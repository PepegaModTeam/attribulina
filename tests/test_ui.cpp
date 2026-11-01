// Runs the editor UI headless against the Pro Street PC files: opening a
// folder, drawing every class's first collection (every value editor), and
// saving through the menus' code paths.

#include "Test.h"

#include "app/App.h"
#include "app/ValueEditors.h"
#include "core/ClassList.h"
#include "core/Clip.h"
#include "core/Compression.h"
#include "core/Edit.h"
#include "core/Folder.h"
#include "core/Game.h"
#include "core/Gameplay.h"
#include "core/Hash.h"
#include "core/Script.h"
#include "core/Writer.h"
#include "core/lua/Lua.h"
#include "platform/headless/HeadlessHost.h"

#include "UiProbe.h"
#include "imgui_impl_null.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <cfloat>
#include <cmath>
#include <memory>
#include <tuple>
#include <unordered_set>

using namespace attrib;

namespace {
std::string ProStreetDir() { return test::DataPath("attrib/databases/pc/prostreet-vanilla"); }

size_t Errors(const app::App& a) {
    size_t n = 0;
    for (const auto& l : a.log())
        if (l.level == app::LogLevel::Error) {
            std::printf("        error: %s\n", l.text.c_str());
            ++n;
        }
    return n;
}
} // namespace

void ProbeBefore();
void ProbeAfter();

TEST("ui: opens the Pro Street folder and draws every class") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe::get().conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    imgui.run(a, 2);

    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    CHECK(db);
    // ATTRIBUTES.BIN, cars_vault.BIN, FE_ATTRIB.BIN and gameplay.lzc;
    // gameplay.bin is skipped.
    CHECK_EQ(db->packs().size(), size_t(4));
    CHECK(host.title.find("Attribulina") == 0);
    imgui.run(a, 2);

    // Everything visible: inherited and unset fields, arrays expanded.
    a.viewOptions() = app::ViewOptions{true, true, true};
    size_t drawn = 0;
    for (const auto& cls : db->classes()) {
        if (cls->collections.empty()) continue;
        a.select(cls->collections.front(), true);
        imgui.run(a, 1);
        ++drawn;
    }
    CHECK(drawn > 100);
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : test::UiProbe::get().conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(test::UiProbe::get().conflicts.empty());
    CHECK(!a.hasUnsavedChanges());
}

TEST("ui: save writes the pack back unchanged") {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "attribulina_ui_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    fs::copy_file(fs::path(ProStreetDir()) / "ATTRIBUTES.BIN", tmp / "ATTRIBUTES.BIN");

    platform::HeadlessImGui imgui;
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    CHECK(a.open((tmp / "ATTRIBUTES.BIN").string()));
    imgui.run(a, 2);
    Database* db = a.database();
    LoadedPack& pack = *db->packs().front();
    pack.dirty = true;
    CHECK(a.saveAll());
    CHECK(!a.hasUnsavedChanges());
    CHECK(ReadFile((tmp / "ATTRIBUTES.BIN").string()) == ReadFile(ProStreetDir() + "/ATTRIBUTES.BIN"));
    // The first save keeps the original beside it.
    CHECK(fs::exists(tmp / "ATTRIBUTES.BIN.bak"));
    CHECK_EQ(Errors(a), size_t(0));
    fs::remove_all(tmp);
}

TEST("ui: Pro Street's console folders, Carbon's, Undercover's, Most Wanted's (its early demos too) and World's open "
     "without warnings, draw every class, and save unchanged") {
    namespace fs = std::filesystem;
    struct Folder {
        const char* path;
        const char* target;
        const char* schema;
        bool unchanged = true; // false: another tool's layout (NFS-VltEd's), which saving doesn't keep
        const char* allowed = nullptr; // a warning the files call for
    };
    for (const Folder& f : {Folder{"360/prostreet-vanilla", "prostreet_x360", "prostreet_pc"},
                            Folder{"ps2/prostreet-vanilla", "prostreet_ps2", "prostreet_ps2"},
                            Folder{"wii/prostreet-vanilla", "prostreet_wii", "prostreet_pc"},
                            Folder{"pc/carbon-vanilla", "carbon_pc", "carbon_pc"},
                            Folder{"ps3/carbon-vanilla", "carbon_ps3", "carbon_pc"},
                            Folder{"wii/carbon-vanilla", "carbon_wii", "carbon_pc"},
                            Folder{"ps2/carbon-vanilla", "carbon_ps2", "carbon_ps2"},
                            Folder{"gamecube/carbon-vanilla", "carbon_gamecube", "carbon_ps2"},
                            Folder{"wii/undercover-vanilla", "undercover_wii", "undercover_wii"},
                            Folder{"360/undercover-vanilla", "undercover_x360", "undercover_pc"},
                            Folder{"pc/undercover-vanilla", "undercover_pc", "undercover_pc", false},
                            Folder{"gamecube/mw-vanilla", "mw_gamecube", "mw_gamecube"},
                            Folder{"xbox/mw-early-oxm-betademo", "mw_xbox", "mw_xbox_demo"},
                            Folder{"ps2/mw-early-opm-betademo", "mw_ps2", "mw_ps2_demo"},
                            // An event of World's sets four fields its class no longer has.
                            Folder{"pc/world-final", "world_pc", "world_pc", true, "which its class doesn't define"}}) {
        const fs::path from = test::DataPath(std::string("attrib/databases/") + f.path);
        if (!fs::exists(from)) continue;
        std::string tmpName = std::string("attribulina_ui_") + f.path;
        std::replace(tmpName.begin(), tmpName.end(), '/', '_');
        const fs::path tmp = fs::temp_directory_path() / tmpName;
        fs::remove_all(tmp);
        fs::create_directories(tmp);
        fs::copy(from, tmp, fs::copy_options::recursive); // World's event vaults are in gc.vaults

        platform::HeadlessImGui imgui;
        imgui.beforeFrame = ProbeBefore;
        imgui.afterFrame = ProbeAfter;
        test::UiProbe::get().conflicts.clear();
        platform::HeadlessHost host;
        app::App a(host);
        a.setup(1.0f, "");
        a.setUserKeysPath("");
        const size_t before = a.log().size();
        CHECK(a.open(tmp.string()));
        Database* db = a.database();
        size_t packs = 0;
        for (const FolderFile& ff : FolderPacks(tmp.string())) packs += ff.skipped.empty();
        CHECK(db && db->packs().size() == packs);
        if (!db) continue;
        CHECK_EQ(db->target().id(), std::string(f.target));
        CHECK(db->typeSchema() && db->typeSchema()->source.find(f.schema) != std::string::npos);
        auto expected = [&](const app::LogLine& l) {
            return l.level == app::LogLevel::Info || (f.allowed && l.level == app::LogLevel::Warning && l.text.find(f.allowed) != std::string::npos);
        };
        for (size_t i = before; i < a.log().size(); ++i)
            if (!expected(a.log()[i])) std::printf("        %s: %s\n", f.path, a.log()[i].text.c_str());
        for (size_t i = before; i < a.log().size(); ++i) CHECK(expected(a.log()[i]));
        a.viewOptions() = app::ViewOptions{true, true, true};
        for (const auto& cls : db->classes()) {
            if (cls->collections.empty()) continue;
            a.select(cls->collections.front(), true);
            imgui.run(a, 1);
        }
        CHECK_EQ(Errors(a), size_t(0));
        for (const auto& c : test::UiProbe::get().conflicts) std::printf("        %s: ID conflict: %s\n", f.path, c.c_str());
        CHECK(test::UiProbe::get().conflicts.empty());
        for (const auto& p : db->packs()) p->dirty = true;
        CHECK(a.saveAll());
        CHECK_EQ(Errors(a), size_t(0));
        for (const auto& e : fs::recursive_directory_iterator(from)) {
            if (!f.unchanged) break;
            if (!e.is_regular_file()) continue;
            // A saved .lzc is compressed again: the pack inside is the same.
            const Bytes saved = ReadFile((tmp / fs::relative(e.path(), from)).string()), original = ReadFile(e.path().string());
            if (ParseLzHeader(original)) CHECK(Decompress(saved) == Decompress(original));
            else CHECK(saved == original);
        }
        fs::remove_all(tmp);
    }
}

void ProbeBefore() { test::UiProbe::get().clear(); }
void ProbeAfter() { test::UiProbe::get().endFrame(); }

namespace {
void Rightclick(platform::HeadlessImGui& imgui, app::App& a, const test::ProbeItem& it) {
    imgui.click(a, (it.rect.Min.x + it.rect.Max.x) * 0.5f, (it.rect.Min.y + it.rect.Max.y) * 0.5f, 1);
    imgui.run(a, 1);
}
void Rightclick(platform::HeadlessImGui& imgui, app::App& a, const test::ProbeItem* it) {
    CHECK(it);
    if (it) Rightclick(imgui, a, *it);
}
} // namespace

TEST("ui: context menus open once, on the clicked item; no ID conflicts") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();

    // Reveal pvehicle/default in the class tree (opens the class), then
    // right-click a sibling: the selection follows and the menu shows once.
    ClassDef* pv = db->findClass(StringHash32("pvehicle"));
    Collection* def = db->findCollection(pv->key, StringHash32("default"));
    a.select(def, true);
    imgui.run(a, 3);
    const test::ProbeItem* node = probe.find("default", "##classes");
    CHECK(node);
    Rightclick(imgui, a, *node);
    CHECK(a.selection() == def);
    CHECK_EQ(probe.count("Rename...", "##Popup"), 1);
    CHECK_EQ(probe.count("New Child Collection...", "##Popup"), 1);
    imgui.mouseMove(1, 1); // click away to close
    imgui.click(a, 1900, 1000);

    // A class node has its own menu.
    const test::ProbeItem* cls = nullptr;
    for (const auto& it : probe.items)
        if (it.label.rfind("pvehicle  (", 0) == 0) cls = &it;
    CHECK(cls);
    Rightclick(imgui, a, *cls);
    CHECK_EQ(probe.count("New Collection...", "##Popup"), 1);
    CHECK_EQ(probe.count("Rename...", "##Popup"), 0);

    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: the Explorer's tree scrolls sideways when a row is wider than it") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    auto tree = []() -> ImGuiWindow* {
        for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
            if (std::string(w->Name).rfind("Explorer/##classes", 0) == 0) return w;
        return nullptr;
    };
    imgui.run(a, 3);
    CHECK(tree() && !tree()->ScrollbarX);
    const std::string longName(300, 'w');
    CHECK(a.runConsole("rename pvehicle/street to " + longName));
    Database* db = a.database();
    a.select(db->findCollection(StringHash32("pvehicle"), StringHash32(longName)), true);
    imgui.run(a, 4);
    CHECK(probe.find(longName, "##classes"));
    CHECK(tree() && tree()->ScrollbarX);
    CHECK(probe.conflicts.empty());
}

TEST("ui: names the user types are kept in user_keys.txt") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "attribulina_keys_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const uint32_t key = StringHash32("attribulina_user_name_test");
    {
        platform::HeadlessImGui imgui;
        platform::HeadlessHost host;
        host.exeDir = dir.string();
        app::App a(host);
        a.setup(1.0f, "");
        CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
        CHECK(a.database()->names().find(key) == nullptr);
        a.rememberName("attribulina_user_name_test");
        a.rememberName("attribulina_user_name_test"); // stored once
        CHECK(a.database()->names().find(key) != nullptr);
    }
    CHECK(fs::exists(dir / "user_keys.txt"));
    CHECK_EQ(ReadFile((dir / "user_keys.txt").string()).size(), std::string("attribulina_user_name_test\n").size());
    {
        // A new session knows the name as soon as a database opens.
        platform::HeadlessImGui imgui;
        platform::HeadlessHost host;
        host.exeDir = dir.string();
        app::App a(host);
        a.setup(1.0f, "");
        CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
        const std::string* n = a.database()->names().find(key);
        CHECK(n && *n == "attribulina_user_name_test");
    }
    fs::remove_all(dir);
}

TEST("ui: every attribute type is drawn expanded at least once") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe::get().conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    CHECK(db->typeSchema() != nullptr); // data/schema/prostreet_pc.txt beside the "exe"
    a.viewOptions() = app::ViewOptions{true, true, true};

    // One collection per attribute type that it sets itself.
    std::unordered_set<const TypeInfo*> covered;
    size_t frames = 0;
    for (const auto& cls : db->classes())
        for (Collection* c : cls->collections) {
            bool fresh = false;
            for (const Attribute& at : c->attributes)
                if (at.type && covered.insert(at.type).second) fresh = true;
            if (!fresh) continue;
            a.select(c, true);
            imgui.run(a, 1);
            ++frames;
        }
    std::printf("        %zu attribute types drawn in %zu frames\n", covered.size(), frames);
    CHECK(covered.size() >= 154); // every type a Pro Street PC collection uses
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : test::UiProbe::get().conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(test::UiProbe::get().conflicts.empty());
}

TEST("ui: colour fields pick the colour editor; the choice is per name") {
    Database db;
    app::DisplayPrefs prefs;
    app::EditEnv env{db, Endian::Little, nullptr, {}, {}, &prefs};
    app::ValueType v;
    v.kind = TypeKind::Vector4;
    v.name = "SunColour";
    CHECK(app::ShowsAsColour(env, v));
    v.name = "FixedFunctionSkyColor";
    CHECK(app::ShowsAsColour(env, v));
    v.name = "ColourCurve"; // coefficients, not a colour
    CHECK(!app::ShowsAsColour(env, v));
    v.name = "VelocityStart";
    CHECK(!app::ShowsAsColour(env, v));
    v.hint = "colour";
    CHECK(app::ShowsAsColour(env, v));
    prefs.colour["VelocityStart"] = false; // the user's choice wins
    CHECK(!app::ShowsAsColour(env, v));
    v.kind = TypeKind::Float;
    v.name = "SunColour";
    CHECK(!app::ShowsAsColour(env, v));
}

TEST("ui: favourites, colour choices, open panels and the window placement persist") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "attribulina_ini_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string ini = (dir / "attribulina.ini").string();
    {
        platform::HeadlessImGui imgui;
        platform::HeadlessHost host;
        auto a = std::make_unique<app::App>(host);
        a->setup(1.0f, ini);
        // The first frame lays the dock space out and sets the default panels, as in the editor.
        imgui.run(*a, 1);
        CHECK(a->open(ProStreetDir() + "/ATTRIBUTES.BIN"));
        Database* db = a->database();
        Collection* sun = db->findCollection(StringHash32("pvehicle"), StringHash32("default"));
        a->select(sun);
        a->toggleFavorite(sun);
        a->displayPrefs().colour["VelocityStart"] = true;
        a->setOpenPanels("Explorer,Search,Script");
        a->setWindowPlacement("0,10,20,810,620");
        a->setDetectTarget(false);
        a->setArraysPastLimit(true);
        a->setSaveOnlyChanged(true);
        imgui.run(*a, 2);
        // The app saves the ini and detaches from it before the ImGui context
        // goes, so DestroyContext() can't save through a stale filename.
        a.reset();
        CHECK(ImGui::GetIO().IniFilename == nullptr);
    }
    {
        platform::HeadlessImGui imgui;
        platform::HeadlessHost host;
        app::App a(host);
        a.setup(1.0f, ini);
        CHECK(!a.detectTarget()); // asks for the game and platform every time
        a.setDetectTarget(true);
        CHECK(a.arraysPastLimit());
        CHECK(a.saveOnlyChanged());
        CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
        Collection* sun = a.database()->findCollection(StringHash32("pvehicle"), StringHash32("default"));
        CHECK(a.isFavorite(sun));
        CHECK(a.displayPrefs().colour.count("VelocityStart") && a.displayPrefs().colour.at("VelocityStart"));
        // The panels that were open, and where the window was.
        CHECK_EQ(a.openPanels(), std::string("Explorer,Search,Script"));
        CHECK_EQ(a.windowPlacement(), std::string("0,10,20,810,620"));
    }
    fs::remove_all(dir);
}

TEST("ui: favourites are kept per database folder, and one the database lacks can still be removed") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "attribulina_favorites_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string ini = (dir / "attribulina.ini").string();
    const fs::path favs = dir / "favorites.txt";
    auto text = [&]() {
        std::ifstream in(favs, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    auto keys = [](uint32_t cls, uint32_t key) {
        char b[24];
        std::snprintf(b, sizeof b, "%08X:%08X", cls, key);
        return std::string(b);
    };
    const std::string carbonDir = test::DataPath("attrib/databases/pc/carbon-vanilla");
    const uint32_t pv = StringHash32("pvehicle"), def = StringHash32("default"), gone = StringHash32("attribulina_gone");
    // An older version kept one list in the .ini for every database.
    { std::ofstream(ini) << "[Attribulina][Settings]\nFavorite=" << keys(pv, def) << "\nFavorite=" << keys(pv, gone) << "\n\n"; }
    {
        platform::HeadlessImGui imgui;
        platform::HeadlessHost host;
        auto a = std::make_unique<app::App>(host);
        a->setup(1.0f, ini);
        CHECK_EQ(text(), "[]\n" + keys(pv, def) + "\n" + keys(pv, gone) + "\n");
        // The first database that has one takes it; the other waits for a database that has it.
        CHECK(a->open(ProStreetDir()));
        CHECK(a->isFavorite(a->database()->findCollection(pv, def)));
        CHECK(text().find("[]\n" + keys(pv, gone) + "\n") != std::string::npos);
        CHECK(text().find("]\n" + keys(pv, def) + " pvehicle/default\n") != std::string::npos);
        // Another database has a list of its own.
        a->closeAll();
        CHECK(a->open(carbonDir));
        Collection* carbonDefault = a->database()->findCollection(pv, def);
        CHECK(carbonDefault);
        CHECK(a->favorites().empty());
        CHECK(!a->isFavorite(carbonDefault));
        a->toggleFavorite(carbonDefault);
        CHECK(a->isFavorite(carbonDefault));
        // A file of the folder opens the same database, with the same list.
        a->closeAll();
        CHECK(a->favorites().empty());
        CHECK(a->open(ProStreetDir() + "/ATTRIBUTES.BIN"));
        CHECK_EQ(a->favorites().size(), size_t(1));
        imgui.run(*a, 2);
        a.reset();
    }
    std::ifstream iniIn(ini);
    const std::string iniText((std::istreambuf_iterator<char>(iniIn)), std::istreambuf_iterator<char>());
    CHECK(iniText.find("Favorite=") == std::string::npos);

    // A favourite the database lacks (deleted, or the files put back outside
    // the editor) is greyed out, and its menu still opens to remove it.
    {
        std::ofstream(favs, std::ios::binary) << "[" << ProStreetDir() << "]\n" << keys(pv, def) << " pvehicle/default\n"
                                              << keys(pv, gone) << " pvehicle/attribulina_gone\n[" << carbonDir << "]\n"
                                              << keys(pv, def) << " pvehicle/default\n";
    }
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, ini);
    CHECK(a.open(ProStreetDir()));
    CHECK_EQ(a.favorites().size(), size_t(2));
    imgui.run(a, 2);
    const test::ProbeItem* missing = probe.find("pvehicle/attribulina_gone", "Explorer");
    CHECK(missing);
    if (missing) {
        Rightclick(imgui, a, *missing);
        const test::ProbeItem* remove = probe.find("Remove from Favorites", "##Popup");
        CHECK(remove);
        if (remove) {
            imgui.click(a, (remove->rect.Min.x + remove->rect.Max.x) * 0.5f, (remove->rect.Min.y + remove->rect.Max.y) * 0.5f);
            imgui.run(a, 1);
        }
    }
    CHECK_EQ(a.favorites().size(), size_t(1));
    CHECK_EQ(text(), "[" + ProStreetDir() + "]\n" + keys(pv, def) + " pvehicle/default\n[" + carbonDir + "]\n" + keys(pv, def) +
        " pvehicle/default\n");
    fs::remove_all(dir);
}

TEST("ui: the recent files move to the top with use and are shared between windows") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "attribulina_recent_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string ini = (dir / "attribulina.ini").string();
    const fs::path recent = dir / "recent_files.txt";
    auto lines = [&]() {
        std::vector<std::string> r;
        std::ifstream in(recent);
        for (std::string l; std::getline(in, l);) r.push_back(l);
        return r;
    };
    const std::string folder = fs::absolute(ProStreetDir()).lexically_normal().string();
    const std::string attrs = (fs::path(folder) / "ATTRIBUTES.BIN").string();
    const std::string other = (dir / "elsewhere").string();
    {
        platform::HeadlessImGui imgui;
        platform::HeadlessHost host;
        auto a = std::make_unique<app::App>(host);
        a->setup(1.0f, ini);
        CHECK(a->open(ProStreetDir() + "/./ATTRIBUTES.BIN"));
        CHECK(lines() == std::vector<std::string>{attrs}); // written right away, normalized
        // Another window opens something meanwhile.
        { std::ofstream(recent) << other << "\n" << attrs << "\n"; }
        imgui.run(*a, 1);
        CHECK(a->open(attrs)); // open already: moves back to the top
        CHECK(lines() == (std::vector<std::string>{attrs, other}));
        a->closeAll();
        CHECK(a->open(ProStreetDir() + "/"));
        CHECK(lines() == (std::vector<std::string>{folder, attrs, other}));
        imgui.run(*a, 1);
        a.reset(); // saves the .ini, which no longer holds the list
    }
    std::ifstream iniIn(ini);
    const std::string iniText((std::istreambuf_iterator<char>(iniIn)), std::istreambuf_iterator<char>());
    CHECK(iniText.find("Recent=") == std::string::npos);
    {
        platform::HeadlessImGui imgui;
        platform::HeadlessHost host;
        app::App a(host);
        a.setup(1.0f, ini);
        CHECK(a.recentFiles() == (std::vector<std::string>{folder, attrs, other}));
    }
    fs::remove_all(dir);
}

TEST("ui: one database per window: another folder asks first, then replaces the open one") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    CHECK(a.open(ProStreetDir()));
    // Something selected and the class tree open, as after normal use.
    Database* db = a.database();
    ClassDef* pv = db->findClass(StringHash32("pvehicle"));
    a.select(db->findCollection(pv->key, StringHash32("default")), true);
    imgui.run(a, 3);
    auto clickCenter = [&](const test::ProbeItem* it) {
        CHECK(it);
        imgui.click(a, (it->rect.Min.x + it->rect.Max.x) * 0.5f, (it->rect.Min.y + it->rect.Max.y) * 0.5f);
        imgui.run(a, 1);
    };

    // Its own folder and files are no conflict; another folder, another
    // database file, and the same vaults from another file are.
    const std::string vlted = test::DataPath("attrib/databases/pc/prostreet-vlted");
    CHECK(!a.conflictWith(ProStreetDir()));
    CHECK(!a.conflictWith(ProStreetDir() + "/gameplay.lzc"));
    CHECK(a.conflictWith(vlted));
    CHECK(a.conflictWith(vlted + "/ATTRIBUTES.BIN"));
    CHECK(a.conflictWith(vlted + "/gameplay.lzc"));
    CHECK(a.conflictWith(ProStreetDir() + "/gameplay.bin")); // gameplay.lzc's vaults
    CHECK(a.open(ProStreetDir()));                           // nothing new to load
    CHECK_EQ(a.database()->packs().size(), size_t(4));

    // Opening another one asks; Open in new window leaves this one alone.
    CHECK(!a.open(vlted));
    imgui.run(a, 2);
    clickCenter(probe.find("Open in new window"));
    CHECK_EQ(host.newWindows.size(), size_t(1));
    CHECK_EQ(host.newWindows.front(), vlted);
    CHECK(a.database() == db);
    CHECK_EQ(db->packs().size(), size_t(4));

    // The menu path: File > Open Folder..., then Open in this window, which frees
    // the database mid-frame, before the Explorer and inspector draw.
    host.nextOpenFolder = vlted;
    clickCenter(probe.find("File"));
    clickCenter(probe.find("Open Folder..."));
    imgui.run(a, 2);
    CHECK(a.database()->packs().front()->path.find("prostreet-vanilla") != std::string::npos);
    clickCenter(probe.find("Open in this window"));
    imgui.run(a, 3);
    CHECK(a.database() != nullptr);
    CHECK(a.database()->packs().front()->path.find("prostreet-vlted") != std::string::npos);
    CHECK(a.selection() == nullptr);
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

namespace {
void ClickItem(platform::HeadlessImGui& imgui, app::App& a, const test::ProbeItem& it, int button = 0) {
    imgui.click(a, (it.rect.Min.x + it.rect.Max.x) * 0.5f, (it.rect.Min.y + it.rect.Max.y) * 0.5f, button);
    imgui.run(a, 1);
}
// For probe.find's result: an item that wasn't drawn fails the test instead of crashing the run.
void ClickItem(platform::HeadlessImGui& imgui, app::App& a, const test::ProbeItem* it, int button = 0) {
    CHECK(it);
    if (it) ClickItem(imgui, a, *it, button);
}

// Script, Script (nfsms) and Log are tabs of one dock node, and only the front
// one draws its contents. Clicks the panel's tab, as a user would.
void FrontTab(platform::HeadlessImGui& imgui, app::App& a, const std::string& panel) {
    ClickItem(imgui, a, test::UiProbe::get().find(panel, "DockSpace"));
}
} // namespace

TEST("ui: the game and platform come from the files; the prompt asks when they don't say, or always when detection is off") {
    namespace fs = std::filesystem;
    namespace sc = attrib::script;
    // Pro Street PS2 files whose Vers value isn't one Attribulina knows, in a
    // folder whose name says nothing.
    const fs::path dir = fs::temp_directory_path() / "attribulina_target_test" / "files";
    fs::remove_all(dir.parent_path());
    fs::create_directories(dir);
    Bytes b = ReadFile(test::DataPath("attrib/databases/ps2/prostreet-vanilla/ATTRIBUTES.BIN"));
    const uint32_t vlt = load32(b.data() + 0x20, Endian::Little);
    for (int i = 8; i < 16; ++i) b[vlt + i] ^= 0x5A;
    WriteFile((dir / "ATTRIBUTES.BIN").string(), b);
    fs::copy_file(test::DataPath("attrib/databases/ps2/prostreet-vanilla/cars_vault.BIN"), dir / "cars_vault.BIN");

    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    auto item = [&](const std::string& prefix) -> const test::ProbeItem* {
        for (const auto& it : probe.items)
            if (it.label.rfind(prefix, 0) == 0 && it.window.find("Game and Platform") != std::string::npos) return &it;
        std::printf("        no item %s\n", prefix.c_str());
        return nullptr;
    };
    auto click = [&](const std::string& prefix) {
        const test::ProbeItem* it = item(prefix);
        CHECK(it);
        if (it) ClickItem(imgui, a, *it);
        imgui.run(a, 1);
    };
    auto opened = [&]() { return a.database() && !a.database()->packs().empty(); };

    // The files say: no prompt.
    CHECK(a.detectTarget());
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    CHECK_EQ(a.database()->target().id(), std::string("prostreet_pc"));
    imgui.run(a, 1);
    CHECK(host.title.find("(Pro Street on PC)") != std::string::npos);
    a.closeAll();

    // They don't: asked, with the best guess (Pro Street's types, 4-byte bools: the PS2).
    CHECK(!a.open((dir / "ATTRIBUTES.BIN").string()));
    CHECK(!opened());
    imgui.run(a, 2);
    CHECK(item("Pro Street  (best guess)###"));
    CHECK(item("PS2  (best guess)###"));
    CHECK(item("World###"));
    CHECK(item("Nitro  (not supported yet)###"));
    CHECK(item("Most Wanted###"));
    // Another file dropped meanwhile joins the database it opens.
    CHECK(!a.open((dir / "cars_vault.BIN").string()));
    click("Open");
    CHECK(opened());
    CHECK_EQ(a.database()->packs().size(), size_t(2));
    CHECK_EQ(a.database()->target().id(), std::string("prostreet_ps2"));
    CHECK(a.database()->typeSchema() && a.database()->typeSchema()->source.find("prostreet_ps2") != std::string::npos);
    a.closeAll();

    // A file that isn't there says so, without asking.
    CHECK(!a.open((dir / "missing" / "attributes.bin").string()));
    CHECK(a.log().back().level == app::LogLevel::Error && a.log().back().text.find("isn't there") != std::string::npos);
    imgui.run(a, 1);
    CHECK(!probe.find("Cancel", "Game and Platform"));

    // Detection off: asked even when the files say. Cancel opens nothing.
    a.setDetectTarget(false);
    CHECK(!a.open(ProStreetDir()));
    imgui.run(a, 2);
    CHECK(item("Pro Street  (detected)###"));
    CHECK(item("PC  (detected)###"));
    click("Cancel");
    CHECK(!opened());
    // Escape cancels too.
    CHECK(!a.open(ProStreetDir()));
    imgui.run(a, 2);
    imgui.key(a, ImGuiKey_Escape);
    imgui.run(a, 1);
    CHECK(!opened());
    // Another game keeps the platform when it came out there; the choice wins.
    CHECK(!a.open(ProStreetDir()));
    imgui.run(a, 2);
    click("Carbon###");
    // Undercover isn't edited on the PS3 (no files seen).
    click("Undercover###");
    CHECK(item("PS3  (not supported yet)###"));
    CHECK(item("PC###") && item("Wii###"));
    click("Pro Street  (detected)###");
    click("Xbox 360###");
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    CHECK(opened());
    CHECK_EQ(a.database()->target().id(), std::string("prostreet_x360"));
    bool warned = false;
    for (const auto& l : a.log()) warned |= l.text.find("as Pro Street on Xbox 360, though these files are little-endian") != std::string::npos;
    CHECK(warned);
    imgui.run(a, 1);
    CHECK(host.title.find("(Pro Street on Xbox 360)") != std::string::npos);
    // Scripts made from it name it.
    a.addToScript({sc::ExportValue(*a.database(), *a.database()->findCollection(StringHash32("audio_revlimiter"), StringHash32("default")),
                                   StringHash32("total_frames_short"))},
                  true);
    imgui.run(a, 1);
    CHECK(a.scriptText().find("game prostreet_x360") != std::string::npos);
    CHECK(probe.conflicts.empty());
    fs::remove_all(dir.parent_path());
}

TEST("ui: tabs: middle-click opens a background tab, each tab keeps its history") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    const KeyNames& names = db->names();

    // Two siblings under the first collection that has them, with the tree
    // open down to them.
    Collection* def = nullptr;
    for (const auto& cls : db->classes())
        for (Collection* c : cls->collections)
            if (!def && c->children.size() >= 2) def = c;
    CHECK(def);
    ClassDef* pv = def->cls;
    std::vector<Collection*> kids = def->children;
    std::sort(kids.begin(), kids.end(), [&](Collection* x, Collection* y) { return names.format(x->key) < names.format(y->key); });
    Collection* first = kids[0];
    Collection* second = kids[1];
    a.select(first, true);
    imgui.run(a, 3);
    CHECK_EQ(a.tabs().size(), size_t(1));

    // Middle-click the sibling: a new tab, but the current one stays in front.
    const test::ProbeItem* node = probe.find(names.format(second->key), "##classes");
    CHECK(node);
    ClickItem(imgui, a, *node, 2);
    CHECK_EQ(a.tabs().size(), size_t(2));
    CHECK(a.selection() == first);
    CHECK(a.tabs()[1].collection == second);

    // Bring it to front by clicking the tab.
    const test::ProbeItem* tab = nullptr;
    for (const auto& it : probe.items)
        if (it.label.rfind(names.format(second->key) + "###tab", 0) == 0) tab = &it;
    CHECK(tab);
    ClickItem(imgui, a, *tab);
    imgui.run(a, 1);
    CHECK(a.selection() == second);
    CHECK_EQ(a.activeTab(), size_t(1));

    // Navigating in this tab doesn't touch the other tab's history.
    a.select(def, true);
    imgui.run(a, 1);
    CHECK_EQ(a.tabs()[1].history.size(), size_t(2));
    CHECK_EQ(a.tabs()[0].history.size(), size_t(1));
    CHECK(a.tabs()[0].collection == first);

    // A tab showing a deleted collection moves to its parent.
    Collection& made = CollectionEditor(*db).create(*first->vault, *pv, "attribulina_tab_test", first);
    a.openInNewTab(&made, true);
    imgui.run(a, 2);
    CHECK(a.selection() == &made);
    const size_t before = a.tabs().size();
    CHECK(a.removeCollection(&made));
    imgui.run(a, 2);
    CHECK_EQ(a.tabs().size(), before);
    CHECK(a.selection() == first);

    // Middle-click on a tab closes it.
    const int closedId = a.tabs()[0].id;
    CHECK(a.tabs()[0].collection == first);
    const test::ProbeItem* t0 = probe.find(names.format(first->key) + "###tab" + std::to_string(closedId));
    CHECK(t0);
    ClickItem(imgui, a, *t0, 2);
    imgui.run(a, 1);
    CHECK_EQ(a.tabs().size(), before - 1);
    for (const auto& t : a.tabs()) CHECK(t.id != closedId);

    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

namespace {
// The item labelled `label` on the same table row as the row named `row`.
const test::ProbeItem* OnRow(const test::UiProbe& probe, const std::string& row, const std::string& label) {
    const test::ProbeItem* r = probe.find(row, "Collection");
    if (!r) return nullptr;
    const float y = (r->rect.Min.y + r->rect.Max.y) * 0.5f;
    for (const auto& it : probe.items)
        if (it.label == label && it.window.find("Collection") != std::string::npos && it.rect.Min.y <= y && y <= it.rect.Max.y)
            return &it;
    return nullptr;
}
} // namespace

TEST("ui: a name typed into a StringKey is remembered once, not each prefix") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "attribulina_stringkey_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        platform::HeadlessImGui imgui;
        imgui.beforeFrame = ProbeBefore;
        imgui.afterFrame = ProbeAfter;
        test::UiProbe& probe = test::UiProbe::get();
        probe.conflicts.clear();
        platform::HeadlessHost host;
        host.exeDir = dir.string();
        app::App a(host);
        a.setup(1.0f, "");
        CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
        Database* db = a.database();
        // The first collection with a StringKey value of its own.
        Collection* col = nullptr;
        std::string field;
        for (const auto& c : db->classes()) {
            for (Collection* k : c->collections)
                for (const Attribute& at : k->attributes) {
                    const std::string* n = db->names().find(at.key);
                    if (!n || at.isArray || TypeOfAttribute(at, *n).kind != TypeKind::StringKey) continue;
                    col = k;
                    field = *n;
                    break;
                }
            if (col) break;
        }
        CHECK(col);
        if (!col) return;
        a.select(col, true);
        imgui.run(a, 2);
        const test::ProbeItem* filter = probe.find("##fieldfilter", "Collection");
        CHECK(filter);
        if (!filter) return;
        ClickItem(imgui, a, *filter);
        imgui.type(a, field.c_str());
        imgui.run(a, 2);
        const test::ProbeItem* box = OnRow(probe, field, "##text");
        CHECK(box);
        if (!box) return;
        ClickItem(imgui, a, *box);
        imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
        for (const char* ch : {"A", "t", "t", "r", "Q"}) imgui.type(a, ch); // a key per frame
        imgui.run(a, 1);
        CHECK(db->names().find(StringHash32("Att")) == nullptr); // still typing
        imgui.key(a, ImGuiKey_Enter);
        imgui.run(a, 2);
        const Attribute* v = col->find(StringHash32(field));
        CHECK(v);
        if (v) {
            const PtrSlot* s = v->data.ptrAt(StringKeyAt(v->elemSize).text);
            CHECK(s && s->text == "AttrQ");
            CHECK_EQ(load32(v->data.bytes.data() + StringKeyAt(v->elemSize).hash32, Endian::Little), StringHash32("AttrQ"));
        }
        CHECK(db->names().find(StringHash32("AttrQ")) != nullptr);
        for (const char* prefix : {"At", "Att", "Attr"}) CHECK(db->names().find(StringHash32(prefix)) == nullptr); // "A" is a known name

        // Escape puts the text back: nothing new to remember.
        auto typeInto = [&](const char* text) {
            const test::ProbeItem* b = OnRow(probe, field, "##text");
            CHECK(b);
            if (!b) return;
            ClickItem(imgui, a, *b);
            imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
            imgui.type(a, text);
            imgui.run(a, 1);
        };
        typeInto("AttrEsc");
        imgui.key(a, ImGuiKey_Escape);
        imgui.run(a, 2);
        CHECK(db->names().find(StringHash32("AttrEsc")) == nullptr);
        // Leaving the box for another collection, which doesn't draw it again, still remembers it.
        typeInto("AttrLeft");
        Collection* other = nullptr;
        for (Collection* k : col->vault->collections)
            if (k != col) { other = k; break; }
        CHECK(other);
        a.select(other, true);
        imgui.run(a, 2);
        CHECK(db->names().find(StringHash32("AttrLeft")) != nullptr);
    }
    const Bytes saved = ReadFile((dir / "user_keys.txt").string());
    CHECK_EQ(std::string(saved.begin(), saved.end()), std::string("AttrQ\nAttrLeft\n"));
    fs::remove_all(dir);
}

TEST("ui: current-gen Undercover's paints get a colour picker while car_paint_swatch/swatch is there") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(test::DataPath("attrib/databases/360/undercover-vanilla")));
    a.viewOptions() = app::ViewOptions{true, true, true};
    auto ride = [&]() { return a.database()->findCollection(StringHash32("presetride"), StringHash32("first_car")); };
    CHECK(ride());
    if (!ride()) return;
    // ColorEdit's button reports no label (nor does an item past the fold):
    // count the paints' rows (the least indented [n]) whose next item is
    // unlabelled. Without a picker it is the open row's first member, mSlotID.
    auto pickers = [&]() {
        auto element = [](const test::ProbeItem& it) {
            return !it.label.empty() && it.label[0] == '[' && it.window.find("Collection/##fields") != std::string::npos;
        };
        float left = FLT_MAX;
        for (const auto& it : probe.items)
            if (element(it)) left = std::min(left, it.rect.Min.x);
        size_t n = 0;
        for (size_t i = 0; i + 1 < probe.items.size(); ++i)
            n += element(probe.items[i]) && probe.items[i].rect.Min.x < left + 1 && probe.items[i + 1].label.empty();
        return n;
    };
    a.select(ride(), true);
    imgui.run(a, 2);
    const test::ProbeItem* filter = probe.find("##fieldfilter", "Collection");
    CHECK(filter);
    if (!filter) return;
    ClickItem(imgui, a, *filter);
    imgui.type(a, "Paints"); // rows below the fold aren't drawn
    imgui.run(a, 2);
    const Attribute* paints = ride()->find(StringHash32("Paints"));
    CHECK(paints);
    if (!paints) return;
    CHECK_EQ(pickers(), size_t(paints->count)); // one per paint
    const std::string summary = app::FormatValue(*a.database(), paints->data, 0, TypeOfAttribute(*paints, "Paints"), Endian::Big);
    CHECK(summary.find("swatch 46 H 216 S 1.00 V 0.59") != std::string::npos);

    // Without the swatches the paints are their members only.
    CHECK(a.runConsole("delete car_paint_swatch/swatch"));
    a.select(ride(), true);
    imgui.run(a, 2);
    CHECK_EQ(pickers(), size_t(0));
    CHECK(app::FormatValue(*a.database(), paints->data, 0, TypeOfAttribute(*paints, "Paints"), Endian::Big).find("swatch 46") ==
          std::string::npos);
    CHECK_EQ(Errors(a), size_t(0));
    CHECK(probe.conflicts.empty());
}

TEST("ui: World's gameplay view shows its elements, the event vaults' too") {
    const std::string dir = test::DataPath("attrib/databases/pc/world-final");
    if (!std::filesystem::exists(dir)) return;
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(dir));
    Database* db = a.database();
    CHECK(db && a.gameplay() != nullptr);
    if (!db) return;
    a.viewOptions() = app::ViewOptions{true, true, true};
    const uint32_t gp = StringHash32("gameplay");
    // An event in gc.vaults/11_1_2_circuit.bin, and an element of gpcore.bin.
    Collection* event = db->findCollection(gp, StringHash32("race_bin_11/11_1_2_circuit"));
    Collection* core = db->findCollection(gp, StringHash32("basicrace"));
    CHECK(event && core);
    if (!event || !core) return;
    CHECK(event->vault && event->vault->pack && event->vault->pack->format == PackFormat::VaultFile);
    a.select(event, true);
    imgui.run(a, 3);
    CHECK(probe.find("11_1_2_circuit", "##gameplay"));
    a.select(core, true);
    imgui.run(a, 3);
    CHECK(probe.find("basicrace", "##gameplay"));
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: gameplay view: folders, references with Go, new element") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath(""); // names it creates stay out of the repo
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    CHECK(a.gameplay() != nullptr);
    const uint32_t gp = StringHash32("gameplay");
    Collection* race = db->findCollection(gp, StringHash32("prostreet_career/bins/race_bin_an_2_1/gr_1"));
    Collection* layout = db->findCollection(gp, StringHash32("tracks/autobahn_gd/autobahn_gd_grip_1"));
    Collection* king = db->findCollection(gp, StringHash32("prostreet_career/opponents/grip_king"));
    CHECK(race && layout && king);

    // Revealing a gameplay element opens its folders in the Gameplay tab.
    a.select(race, true);
    imgui.run(a, 3);
    CHECK(probe.find("gr_1", "##gameplay"));
    CHECK(probe.find("race_bin_an_2_1", "##gameplay"));

    // TrackLayout is a reference: Go follows it. (The field filter brings the
    // rows into view.)
    auto filterFields = [&](const char* text) {
        const test::ProbeItem* f = probe.find("##fieldfilter", "Collection");
        CHECK(f);
        ClickItem(imgui, a, *f);
        imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
        imgui.type(a, text);
        imgui.run(a, 2);
    };
    filterFields("Track");
    const test::ProbeItem* go = OnRow(probe, "TrackLayout", "Go");
    CHECK(go);
    ClickItem(imgui, a, *go);
    CHECK(a.selection() == layout);

    // TrackRecordHolder (a Text holding a path) too; middle-click opens a tab.
    a.select(race, true);
    imgui.run(a, 2);
    filterFields("Track");
    const size_t tabs = a.tabs().size();
    const test::ProbeItem* go2 = OnRow(probe, "TrackRecordHolder", "Go");
    CHECK(go2);
    ClickItem(imgui, a, *go2, 2);
    CHECK_EQ(a.tabs().size(), tabs + 1);
    CHECK(a.tabs()[a.activeTab() + 1].collection == king);
    CHECK(a.selection() == race);

    // The folder's context menu has the gameplay commands, once.
    const test::ProbeItem* node = probe.find("gr_1", "##gameplay");
    CHECK(node);
    Rightclick(imgui, a, *node);
    CHECK_EQ(probe.count("Duplicate...", "##Popup"), 1);
    CHECK_EQ(probe.count("New Element Here...", "##Popup"), 1);
    imgui.key(a, ImGuiKey_Escape);

    // New Element Here... on the bin: type a name, Enter. The template
    // defaults to what the bin's races use most.
    Collection* bin = db->findCollection(gp, StringHash32("prostreet_career/bins/race_bin_an_2_1"));
    a.select(bin, true);
    imgui.run(a, 2);
    const test::ProbeItem* newButton = probe.find("New Element Here...", "Collection");
    CHECK(newButton);
    ClickItem(imgui, a, *newButton);
    imgui.run(a, 2);
    imgui.type(a, "gr_9");
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    Collection* made = db->findCollection(gp, StringHash32("prostreet_career/bins/race_bin_an_2_1/gr_9"));
    CHECK(made);
    CHECK(a.selection() == made);
    CHECK(made->vault == race->vault);
    CHECK(made->parent != nullptr);
    CHECK(a.gameplay()->owner(made) == bin);
    CHECK_EQ(Errors(a), size_t(0));

    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: sequence builder: decompile a handler, edit its transition, compile, add a handler") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath(""); // names it creates stay out of the repo
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    const uint32_t gp = StringHash32("gameplay");
    Collection* h = db->findCollection(gp, StringHash32("basicrace/stategraph_basicrace_racing_handler_notifyescaped"));
    CHECK(h);
    CHECK(!a.messages().empty());

    a.openStateGraphBuilder(h);
    imgui.run(a, 3);
    CHECK(a.sequenceActivity() == db->findCollection(gp, StringHash32("basicrace")));
    CHECK(a.sequenceState() == db->findCollection(gp, StringHash32("stategraph_basicrace_racing")));
    CHECK(a.sequenceHandler() == h);
    CHECK(a.sequenceStatus().find("same bytecode") != std::string::npos);
    std::string& src = a.sequenceSource();
    CHECK(src.find("h_8C272D2D") != std::string::npos);
    const size_t at = src.find("\"abandoned\"");
    CHECK(at != std::string::npos);

    // Escaping now goes to "done": the compiled handler and its transition follow.
    src.replace(at, 11, "\"done\"");
    CHECK(a.compileHandler());
    const Attribute* tl = h->find(StringHash32("transitionlist"));
    CHECK(tl && tl->count == 1);
    CHECK_EQ(load32(tl->elem(0), Endian::Little), StringHash32("stategraph_basicrace_done"));
    const Bytes chunk = HandlerChunk(*h);
    CHECK(lua::Decompile(chunk).source.find("\"done\"") != std::string::npos);

    // The game's functions, listed beside the code (data/lua).
    const test::ProbeItem* fn = probe.find("Functions", "Sequence Builder");
    CHECK(fn);
    ClickItem(imgui, a, *fn);
    imgui.run(a, 2);
    const test::ProbeItem* filter = probe.find("##filter", "Sequence Builder");
    CHECK(filter);
    ClickItem(imgui, a, *filter);
    imgui.type(a, "print");
    imgui.run(a, 2);
    CHECK(probe.find("Debug.Print (text: string)", "Sequence Builder"));
    CHECK(probe.find("print", "Sequence Builder")); // listed, as missing in the game
    CHECK(probe.find("Your Functions...", "Sequence Builder"));
    ClickItem(imgui, a, probe.find("Functions", "Sequence Builder")); // closed again, and the keyboard leaves the filter
    imgui.run(a, 2);

    // A compile error marks its line and says why.
    const std::string before = src;
    src += "\nthis is not lua\n";
    CHECK(!a.compileHandler());
    CHECK(a.sequenceStatus().find("line ") != std::string::npos);
    src = before;

    // A script that doesn't define the handler's function is refused.
    const std::string good = src;
    src = "function h_00000000( self, message, vars ) end\n";
    CHECK(!a.compileHandler());
    CHECK(a.sequenceStatus().find("h_8C272D2D") != std::string::npos);
    src = good;

    // Saved and read back, the new bytecode is there.
    {
        LoadedPack& pk = *h->vault->pack;
        Database fresh;
        fresh.loadPack(test::DataPath("attrib/databases/pc/prostreet-vanilla/ATTRIBUTES.BIN"));
        fresh.loadPack("gameplay.bin", WritePack(*db, pk));
        Collection* h2 = fresh.findCollection(gp, h->key);
        CHECK(h2 && HandlerChunk(*h2) == chunk);
    }

    // New Handler... in the racing state, for MNotifyEscaped: it gets the
    // next free name and a compiled stub defining its function.
    const test::ProbeItem* nh = probe.find("New Handler...", "Sequence Builder");
    CHECK(nh);
    ClickItem(imgui, a, *nh);
    imgui.run(a, 2);
    imgui.type(a, "NotifyEscaped");
    imgui.run(a, 2);
    const test::ProbeItem* msg = probe.find("MNotifyEscaped", "New Handler");
    CHECK(msg);
    ClickItem(imgui, a, *msg);
    const test::ProbeItem* create = probe.find("Create", "New Handler");
    CHECK(create);
    ClickItem(imgui, a, *create);
    imgui.run(a, 2);
    Collection* made = db->findCollection(gp, StringHash32("basicrace/stategraph_basicrace_racing_handler_notifyescaped_2"));
    CHECK(made);
    CHECK(a.sequenceHandler() == made);
    const Attribute* mid = made->find(StringHash32("message_id"));
    CHECK(mid && load32(mid->elem(0), Endian::Little) == StringHash32("MNotifyEscaped"));
    const lua::ChunkInfo info = lua::Inspect(HandlerChunk(*made));
    CHECK(std::find(info.globalsSet.begin(), info.globalsSet.end(),
                    lua::HandlerFunctionName("basicrace/stategraph_basicrace_racing_handler_notifyescaped_2")) != info.globalsSet.end());
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

namespace {
// A parent outside the gameplay class with at least two leaf children, the
// children sorted the way the class tree shows them.
std::pair<Collection*, std::vector<Collection*>> ParentWithLeaves(Database& db) {
    const KeyNames& names = db.names();
    for (const auto& cls : db.classes()) {
        if (cls->key == StringHash32("gameplay")) continue;
        for (Collection* c : cls->collections) {
            std::vector<Collection*> kids = c->children;
            if (kids.size() < 2 || kids.size() > 40) continue;
            std::sort(kids.begin(), kids.end(), [&](Collection* x, Collection* y) {
                return app::ToLower(names.format(x->key)) < app::ToLower(names.format(y->key));
            });
            if (kids[0]->children.empty() && kids[1]->children.empty() && names.find(kids[0]->key) && names.find(kids[1]->key))
                return {c, kids};
        }
    }
    return {nullptr, {}};
}

std::string Clipboard() { return ImGui::GetClipboardText() ? ImGui::GetClipboardText() : ""; }
} // namespace

TEST("ui: sequence builder: the graph runs top to bottom from initial; Carbon's career_game shows the initial it runs from career") {
    namespace fs = std::filesystem;
    const std::string dir = test::DataPath("attrib/databases/pc/carbon-vanilla");
    if (!fs::exists(dir)) return;
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(dir));
    Database* db = a.database();
    const uint32_t gp = StringHash32("gameplay");
    Collection* game = db->findCollection(gp, StringHash32("career_game"));
    Collection* initial = db->findCollection(gp, StringHash32("stategraph_career_game_initial"));
    Collection* monitor = db->findCollection(gp, StringHash32("stategraph_career_game_monitor"));
    CHECK(game && initial && monitor);
    a.openStateGraphBuilder(game);
    imgui.run(a, 3);
    CHECK(a.sequenceActivity() == game);
    CHECK(a.sequenceState() == initial);
    const auto& boxes = a.sequenceGraph();
    CHECK_EQ(boxes.size(), size_t(5));
    const app::App::GraphBox* init = nullptr;
    const app::App::GraphBox* mon = nullptr;
    for (const auto& b : boxes) {
        if (b.state == initial) init = &b;
        if (b.state == monitor) mon = &b;
    }
    CHECK(init && mon);
    // The first row holds initial alone; the rest are below it.
    for (const auto& b : boxes)
        if (init && b.state != initial) CHECK(b.min.y > init->max.y);
    // A click on a box picks its state.
    if (mon) imgui.click(a, (mon->min.x + mon->max.x) * 0.5f, (mon->min.y + mon->max.y) * 0.5f);
    imgui.run(a, 1);
    CHECK(a.sequenceState() == monitor);
    // Opened from the state itself, it shows the activity its name gives, not career (its templateref).
    a.openStateGraphBuilder(initial);
    imgui.run(a, 2);
    CHECK(a.sequenceActivity() == game);
    CHECK_EQ(a.sequenceGraph().size(), size_t(5));
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: a tree node's menu that changes the tree (Duplicate, Paste) stops the walk over the old lists") {
    // The menu runs while the tree is drawn, and the change rebuilds the
    // sorted lists the walk is in (a crash duplicating Carbon's
    // pvehicle/.../tuner/240sx). Every Explorer list is checked.
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    const KeyNames& names = db->names();
    auto [parent, kids] = ParentWithLeaves(*db);
    CHECK(parent);
    if (!parent) return;
    const std::string firstName = names.format(kids[0]->key);
    a.select(kids[0], true);
    imgui.run(a, 3);
    auto menuItem = [&](const char* label) {
        const test::ProbeItem* n = probe.find(firstName, "##classes");
        CHECK(n);
        if (!n) return;
        ClickItem(imgui, a, *n, 1);
        imgui.run(a, 1);
        const test::ProbeItem* item = probe.find(label, "##Popup");
        CHECK(item);
        if (item) ClickItem(imgui, a, *item);
        imgui.run(a, 2);
    };
    menuItem("Duplicate");
    CHECK(db->findCollection(kids[0]->classKey, StringHash32(firstName + "_copy")));
    menuItem("Copy");
    menuItem("Paste");
    CHECK(db->findCollection(kids[0]->classKey, StringHash32(firstName + "_copy2")));
    // The filtered list too.
    const std::string label = names.format(kids[0]->classKey) + "/" + firstName;
    const test::ProbeItem* filter = probe.find("##filter", "Explorer");
    CHECK(filter);
    if (filter) {
        ClickItem(imgui, a, *filter);
        imgui.type(a, label.c_str());
        imgui.run(a, 2);
    }
    const test::ProbeItem* hit = probe.find(label, "##hits");
    CHECK(hit);
    if (hit) {
        ClickItem(imgui, a, *hit, 1);
        imgui.run(a, 1);
        const test::ProbeItem* item = probe.find("Duplicate", "##Popup");
        CHECK(item);
        if (item) ClickItem(imgui, a, *item);
        imgui.run(a, 2);
    }
    CHECK(db->findCollection(kids[0]->classKey, StringHash32(firstName + "_copy3")));
    CHECK_EQ(Errors(a), size_t(0));
    CHECK(probe.conflicts.empty());

    // The case that crashed: Carbon's pvehicle/default/cars/racers/tuner/240sx.
    const std::string carbon = test::DataPath("attrib/databases/pc/carbon-vanilla");
    if (!std::filesystem::exists(carbon)) return;
    if (const test::ProbeItem* f = probe.find("##filter", "Explorer")) {
        ClickItem(imgui, a, *f);
        imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
        imgui.key(a, ImGuiKey_Delete); // (Escape would put the text back)
    }
    a.closeAll();
    app::App& b = a;
    CHECK(b.open(carbon));
    Collection* sx = b.database()->findCollection(StringHash32("pvehicle"), StringHash32("240sx"));
    CHECK(sx);
    if (!sx) return;
    b.select(sx, true);
    imgui.run(b, 3);
    const test::ProbeItem* n = probe.find("240sx", "##classes");
    CHECK(n);
    if (!n) return;
    ClickItem(imgui, b, *n, 1);
    imgui.run(b, 1);
    const test::ProbeItem* dup = probe.find("Duplicate", "##Popup");
    CHECK(dup);
    if (dup) ClickItem(imgui, b, *dup);
    imgui.run(b, 2);
    Collection* copy = b.database()->findCollection(StringHash32("pvehicle"), StringHash32("240sx_copy"));
    CHECK(copy && copy->parent == sx->parent);
    CHECK_EQ(Errors(b), size_t(0));
}

TEST("ui: Explorer keys: Enter opens, Ctrl+C/V/X/D, F2, Delete act on the focused item only") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    const KeyNames& names = db->names();
    auto [parent, kids] = ParentWithLeaves(*db);
    CHECK(parent);
    Collection* first = kids[0];
    Collection* second = kids[1];
    const std::string secondName = names.format(second->key);
    a.select(first, true);
    imgui.run(a, 3);
    auto node = [&](const std::string& label) { return probe.find(label, "##classes"); };

    // Click gives the node the keyboard; Down moves it without opening; Enter opens.
    const test::ProbeItem* n1 = node(names.format(first->key));
    CHECK(n1);
    ClickItem(imgui, a, *n1);
    CHECK(a.selection() == first);
    imgui.key(a, ImGuiKey_DownArrow);
    CHECK(a.selection() == first);
    CHECK(a.editTarget().collection == second);
    imgui.key(a, ImGuiKey_Enter);
    CHECK(a.selection() == second);
    // Enter on the parent opens it and leaves it expanded.
    const test::ProbeItem* np = node(names.format(parent->key));
    CHECK(np);
    ClickItem(imgui, a, *np);
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 1);
    CHECK(a.selection() == parent);
    CHECK(node(secondName));

    // Ctrl+C / Ctrl+V on the focused sibling: a copy next to it, with a free name.
    ClickItem(imgui, a, *node(secondName));
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_C);
    CHECK(Clipboard().rfind("Attribulina collections 1", 0) == 0);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_V);
    imgui.run(a, 2);
    Collection* copy = db->findCollection(second->classKey, StringHash32(secondName + "_copy"));
    CHECK(copy);
    CHECK(copy->parent == parent);
    CHECK(a.selection() == copy);

    // In a text field the keys are the field's: copying text copies text.
    const size_t count = db->collectionCount();
    const test::ProbeItem* filter = probe.find("##fieldfilter", "Collection");
    CHECK(filter);
    ClickItem(imgui, a, *filter);
    imgui.type(a, "zz");
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_C);
    CHECK_EQ(Clipboard(), std::string("zz"));
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_V);
    imgui.key(a, ImGuiKey_Delete);
    CHECK_EQ(db->collectionCount(), count);
    imgui.key(a, ImGuiKey_Escape);
    imgui.run(a, 2);

    // F2 renames the focused copy; Delete asks, Enter confirms.
    const test::ProbeItem* nc = node(secondName + "_copy");
    CHECK(nc);
    ClickItem(imgui, a, *nc);
    imgui.key(a, ImGuiKey_F2);
    imgui.run(a, 2);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
    imgui.type(a, "attribulina_keys_test");
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    CHECK(copy->key == StringHash32("attribulina_keys_test"));
    const test::ProbeItem* nr = node("attribulina_keys_test");
    CHECK(nr);
    ClickItem(imgui, a, *nr);
    imgui.key(a, ImGuiKey_Delete);
    imgui.run(a, 2);
    CHECK(probe.find("Delete", "Delete Collection"));
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    CHECK(!db->findCollection(second->classKey, StringHash32("attribulina_keys_test")));
    CHECK_EQ(db->collectionCount(), count - 1);

    // Ctrl+D duplicates without touching the clipboard.
    const std::string clip = Clipboard();
    ClickItem(imgui, a, *node(secondName));
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_D);
    imgui.run(a, 2);
    CHECK(db->findCollection(second->classKey, StringHash32(secondName + "_copy")));
    CHECK_EQ(Clipboard(), clip);

    // Ctrl+X marks the first sibling; Ctrl+Shift+V on the second moves it there.
    ClickItem(imgui, a, *node(names.format(first->key)));
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_X);
    CHECK(a.isCut(first));
    CHECK(first->parent == parent);
    ClickItem(imgui, a, *node(secondName));
    imgui.key(a, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V);
    imgui.run(a, 2);
    CHECK(first->parent == second);
    CHECK(!a.isCut(first));
    CHECK(a.hasUnsavedChanges());

    // The Edit menu names what it acts on.
    const test::ProbeItem* edit = probe.find("Edit");
    CHECK(edit);
    ClickItem(imgui, a, *edit);
    imgui.run(a, 1);
    CHECK(probe.find("Paste as Child"));
    imgui.key(a, ImGuiKey_Escape);
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: a collection copied in another window (another database) pastes here as a copy") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    const KeyNames& names = db->names();
    auto [parent, kids] = ParentWithLeaves(*db);
    CHECK(parent);
    Collection* target = kids[0];
    const std::string name = names.format(target->key);

    // The other window: the vlted database, with its own copy of the collection.
    Database other;
    other.names().loadFile(test::DataPath("data/attrib_keys.txt"));
    other.loadPack(test::DataPath("attrib/databases/pc/prostreet-vlted/ATTRIBUTES.BIN"));
    const Collection* theirs = other.findCollection(target->classKey, target->key);
    CHECK(theirs);
    Clip clip = CopyCollections(other, {theirs}, false);
    clip.source = a.instanceId() + 1;
    clip.cut = true; // a cut there is a copy here
    clip.origin = "the other window";
    ImGui::SetClipboardText(EncodeClip(clip).c_str());

    a.select(target, true);
    imgui.run(a, 3);
    const test::ProbeItem* n = probe.find(name, "##classes");
    CHECK(n);
    ClickItem(imgui, a, *n);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_V);
    imgui.run(a, 2);
    Collection* copy = db->findCollection(target->classKey, StringHash32(name + "_copy"));
    CHECK(copy);
    CHECK(copy->parent == parent);
    CHECK(target->parent == parent); // nothing moved
    bool fromOther = false, cutNote = false;
    for (const auto& l : a.log()) {
        fromOther |= l.text.find("copied from the other window") != std::string::npos;
        cutNote |= l.text.find("cut in another window") != std::string::npos;
    }
    CHECK(fromOther && cutNote);
    // Values came from the other database.
    CHECK_EQ(copy->attributes.size(), theirs->attributes.size());
    for (const Attribute& x : theirs->attributes) {
        const Attribute* y = copy->find(x.key);
        CHECK(y && y->data.bytes == x.data.bytes);
    }
    CHECK_EQ(Errors(a), size_t(0));
}

TEST("ui: dragging a collection onto another asks, then moves it there") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    const KeyNames& names = db->names();
    auto [parent, kids] = ParentWithLeaves(*db);
    CHECK(parent);
    Collection* first = kids[0];
    Collection* second = kids[1];
    a.select(first, true);
    imgui.run(a, 3);
    const test::ProbeItem* from = probe.find(names.format(first->key), "##classes");
    const test::ProbeItem* to = probe.find(names.format(second->key), "##classes");
    CHECK(from && to);
    const ImVec2 a0((from->rect.Min.x + from->rect.Max.x) * 0.5f, (from->rect.Min.y + from->rect.Max.y) * 0.5f);
    const ImVec2 b0((to->rect.Min.x + to->rect.Max.x) * 0.5f, (to->rect.Min.y + to->rect.Max.y) * 0.5f);
    imgui.mouseMove(a0.x, a0.y);
    imgui.run(a, 1);
    imgui.mouseButton(0, true);
    imgui.run(a, 1);
    for (int i = 1; i <= 6; ++i) {
        imgui.mouseMove(a0.x + (b0.x - a0.x) * i / 6.0f + 2, a0.y + (b0.y - a0.y) * i / 6.0f);
        imgui.run(a, 1);
    }
    imgui.mouseButton(0, false);
    imgui.run(a, 2);
    CHECK(first->parent == parent); // nothing yet: the drop asks first
    const std::string move = "Move under " + names.format(second->key);
    const test::ProbeItem* item = probe.find(move);
    CHECK(item);
    ClickItem(imgui, a, *item);
    imgui.run(a, 1);
    CHECK(first->parent == second);
    CHECK(a.hasUnsavedChanges());
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: in the gameplay tree the keys run the gameplay commands") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    Collection* race = db->findCollection(StringHash32("gameplay"), StringHash32("prostreet_career/bins/race_bin_an_2_1/gr_1"));
    CHECK(race);
    a.select(race, true);
    imgui.run(a, 3);
    const test::ProbeItem* n = probe.find("gr_1", "##gameplay");
    CHECK(n);
    ClickItem(imgui, a, *n);
    CHECK(a.editTarget().kind == app::App::EditTarget::Kind::Element);

    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_D);
    imgui.run(a, 2);
    CHECK(probe.find("Duplicate", "Duplicate Element"));
    imgui.key(a, ImGuiKey_Escape);
    imgui.run(a, 2);
    ClickItem(imgui, a, probe.find("gr_1", "##gameplay"));
    imgui.key(a, ImGuiKey_Delete);
    imgui.run(a, 2);
    CHECK(probe.find("Delete", "Delete Element"));
    imgui.key(a, ImGuiKey_Escape);
    imgui.run(a, 2);
    CHECK(db->findCollection(race->classKey, race->key) == race);
    // Copy, then paste: a duplicate of the element, with the gameplay conventions.
    ClickItem(imgui, a, probe.find("gr_1", "##gameplay"));
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_C);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_V);
    imgui.run(a, 2);
    CHECK(probe.find("Duplicate", "Duplicate Element"));
    imgui.key(a, ImGuiKey_Escape);
    imgui.run(a, 2);
    // Cut and rename aren't there for elements yet: they say so and do nothing.
    ClickItem(imgui, a, probe.find("gr_1", "##gameplay"));
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_X);
    CHECK(!a.isCut(race));
    CHECK(!a.hasUnsavedChanges());
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: Expand All and Collapse All: Shift+A or a node's menu act on it and all below it, a tree's own menu on the tree; "
     "the filtered list has none") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    const KeyNames& names = db->names();
    const uint32_t gp = StringHash32("gameplay");
    // A class whose root has a child with children, all named.
    ClassDef* cls = nullptr;
    Collection *root = nullptr, *mid = nullptr, *leaf = nullptr;
    for (const auto& c : db->classes()) {
        if (c->key == gp || cls) continue;
        for (Collection* r : c->collections)
            if (!r->parent && names.find(r->key) && !cls)
                for (Collection* m : r->children)
                    if (!m->children.empty() && names.find(m->key) && names.find(m->children[0]->key) && !cls) {
                        cls = c.get();
                        root = r;
                        mid = m;
                        leaf = m->children[0];
                    }
    }
    CHECK(cls && root && mid && leaf);
    const std::string rootName = names.format(root->key), midName = names.format(mid->key), leafName = names.format(leaf->key);
    auto node = [&](const std::string& label) { return probe.find(label, "##classes"); };
    auto popupHas = [&](const char* item) { return probe.count(item, "##Popup") == 1; };
    auto clickPopup = [&](const char* item) {
        const test::ProbeItem* it = probe.find(item, "##Popup");
        CHECK(it);
        if (it) ClickItem(imgui, a, *it);
        imgui.run(a, 2);
    };
    imgui.run(a, 3);
    const std::string classLabel = names.format(cls->key) + "  (" + std::to_string(cls->collections.size()) + ")";
    CHECK(node(classLabel));
    CHECK(!node(rootName));

    // A class node's menu: Expand All opens it and everything under it.
    Rightclick(imgui, a, *node(classLabel));
    CHECK(popupHas("Expand All") && !popupHas("Collapse All"));
    clickPopup("Expand All");
    CHECK(node(rootName) && node(midName) && node(leafName));
    Rightclick(imgui, a, *node(classLabel));
    CHECK(popupHas("Collapse All") && !popupHas("Expand All"));
    clickPopup("Collapse All");
    CHECK(!node(rootName));
    // Opened again on its own, what was under it stayed closed.
    ClickItem(imgui, a, *node(classLabel));
    imgui.run(a, 1);
    CHECK(node(rootName) && !node(midName));

    // Shift+A on the node with the keyboard: open, everything; again, closed.
    ClickItem(imgui, a, *node(rootName));
    CHECK(a.selection() == root);
    imgui.key(a, ImGuiMod_Shift | ImGuiKey_A);
    imgui.run(a, 1);
    CHECK(node(midName) && node(leafName));
    imgui.key(a, ImGuiMod_Shift | ImGuiKey_A);
    imgui.run(a, 1);
    CHECK(node(rootName) && !node(midName));
    imgui.key(a, ImGuiKey_RightArrow); // one level
    imgui.run(a, 1);
    CHECK(node(midName) && !node(leafName));
    // A collection node's menu has it too; a leaf's doesn't.
    Rightclick(imgui, a, *node(rootName));
    CHECK(popupHas("Collapse All"));
    imgui.key(a, ImGuiKey_Escape);
    imgui.run(a, 1);
    CHECK(!a.hasUnsavedChanges());

    // The Files tab: a pack's node and the tree's own menu (right-click below the nodes).
    ClickItem(imgui, a, probe.find("Files", "Explorer"));
    imgui.run(a, 2);
    const LoadedPack& first = *db->packs().front();
    auto vaultLabel = [](const Vault& v) { return v.name + "  (" + std::to_string(v.collections.size()) + ")"; };
    auto filesWindow = []() -> ImGuiWindow* {
        for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
            if (std::string(w->Name).rfind("Explorer/##files", 0) == 0) return w;
        return nullptr;
    };
    CHECK(filesWindow());
    auto background = [&]() {
        const ImRect r = filesWindow()->Rect();
        imgui.click(a, r.Min.x + 30, r.Max.y - 30, 1);
        imgui.run(a, 1);
    };
    CHECK(!probe.find(vaultLabel(*first.vaults.front()), "##files"));
    background();
    CHECK(popupHas("Expand All") && popupHas("Collapse All"));
    clickPopup("Expand All");
    for (const auto& pk : db->packs())
        for (const auto& v : pk->vaults) CHECK(probe.find(vaultLabel(*v), "##files"));
    const std::string firstLabel = std::filesystem::path(first.path).filename().string() + "  (" + std::to_string(first.vaults.size()) + " vaults)";
    CHECK(probe.find(firstLabel, "##files"));
    Rightclick(imgui, a, probe.find(firstLabel, "##files"));
    clickPopup("Collapse All");
    CHECK(!probe.find(vaultLabel(*first.vaults.front()), "##files"));
    CHECK(probe.find(vaultLabel(*db->packs().back()->vaults.front()), "##files"));

    // The gameplay tree: Shift+A on a folder closes and opens all of it.
    Collection* race = db->findCollection(gp, StringHash32("prostreet_career/bins/race_bin_an_2_1/gr_1"));
    CHECK(race);
    a.select(race, true);
    imgui.run(a, 3);
    const test::ProbeItem* career = probe.find("prostreet_career", "##gameplay");
    CHECK(career && probe.find("gr_1", "##gameplay"));
    ClickItem(imgui, a, *career);
    imgui.key(a, ImGuiMod_Shift | ImGuiKey_A);
    imgui.run(a, 1);
    CHECK(probe.find("prostreet_career", "##gameplay") && !probe.find("bins", "##gameplay"));
    imgui.key(a, ImGuiMod_Shift | ImGuiKey_A);
    imgui.run(a, 1);
    CHECK(probe.find("gr_1", "##gameplay") && probe.find("race_bin_an_2_2", "##gameplay"));

    // The filtered list is no tree.
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_F);
    imgui.type(a, rootName.c_str());
    imgui.run(a, 2);
    const test::ProbeItem* hit = nullptr;
    for (const auto& it : probe.items)
        if (!hit && it.window.find("##hits") != std::string::npos && it.label.find(rootName) != std::string::npos) hit = &it;
    CHECK(hit);
    if (hit) Rightclick(imgui, a, *hit);
    CHECK(popupHas("Open in New Tab") && !popupHas("Expand All") && !popupHas("Collapse All"));
    imgui.key(a, ImGuiKey_Escape);
    imgui.run(a, 1);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_F); // empty the filter: the tabs come back
    imgui.run(a, 2);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
    imgui.key(a, ImGuiKey_Backspace);
    imgui.run(a, 1);
    auto clickTab = [&](const char* tab) {
        const test::ProbeItem* it = probe.find(tab, "Explorer");
        CHECK(it);
        if (it) ClickItem(imgui, a, *it);
        imgui.run(a, 2);
        return it != nullptr;
    };

    // Another database (here the same folder again) starts with every tree closed.
    if (!clickTab("Classes")) return;
    CHECK(node(midName));
    a.closeAll();
    CHECK(a.open(ProStreetDir()));
    imgui.run(a, 3);
    CHECK(node(classLabel) && !node(rootName));
    clickTab("Gameplay");
    CHECK(probe.find("prostreet_career", "##gameplay") && !probe.find("bins", "##gameplay"));
    clickTab("Files");
    CHECK(probe.find(firstLabel, "##files") && !probe.find(vaultLabel(*a.database()->packs().back()->vaults.front()), "##files"));
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: Go keys: Alt+Left/Right walk the history, Ctrl+T/PgDn/W manage tabs, Ctrl+F filters") {
    platform::HeadlessImGui imgui;
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    auto [parent, kids] = ParentWithLeaves(*db);
    CHECK(parent);
    a.select(kids[0], true);
    imgui.run(a, 1);
    a.select(kids[1], true);
    imgui.run(a, 2);
    imgui.key(a, ImGuiMod_Alt | ImGuiKey_LeftArrow);
    CHECK(a.selection() == kids[0]);
    imgui.key(a, ImGuiMod_Alt | ImGuiKey_RightArrow);
    CHECK(a.selection() == kids[1]);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_T);
    imgui.run(a, 1);
    CHECK_EQ(a.tabs().size(), size_t(2));
    CHECK_EQ(a.activeTab(), size_t(1));
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_PageDown);
    imgui.run(a, 1);
    CHECK_EQ(a.activeTab(), size_t(0));
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_W);
    imgui.run(a, 1);
    CHECK_EQ(a.tabs().size(), size_t(1));
    CHECK(!ImGui::GetIO().WantTextInput);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_F);
    imgui.run(a, 1);
    CHECK(ImGui::GetIO().WantTextInput);
    CHECK_EQ(Errors(a), size_t(0));
}

TEST("ui: right-clicking Back or Forward lists the way there, nearest first") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    const KeyNames& names = db->names();
    auto [parent, kids] = ParentWithLeaves(*db);
    CHECK(parent);
    for (Collection* c : {parent, kids[0], kids[1]}) {
        a.select(c, true);
        imgui.run(a, 1);
    }
    imgui.key(a, ImGuiMod_Alt | ImGuiKey_LeftArrow);
    imgui.key(a, ImGuiMod_Alt | ImGuiKey_LeftArrow);
    imgui.run(a, 1);
    CHECK(a.selection() == parent);
    // Forward lists kids[0] then kids[1]; picking the far one goes straight there.
    const test::ProbeItem* fwd = probe.find("##forward");
    CHECK(fwd);
    ClickItem(imgui, a, *fwd, 1);
    imgui.run(a, 1);
    const test::ProbeItem* near = probe.find(names.format(kids[0]->key), "##Popup");
    const test::ProbeItem* far = probe.find(names.format(kids[1]->key), "##Popup");
    CHECK(near && far);
    if (!near || !far) return;
    CHECK(near->rect.Min.y < far->rect.Min.y);
    ClickItem(imgui, a, *far);
    imgui.run(a, 1);
    CHECK(a.selection() == kids[1]);
    CHECK_EQ(a.tabs()[0].history.size(), size_t(3)); // jumping keeps the history
    // Back lists kids[0] first.
    ClickItem(imgui, a, probe.find("##back"), 1);
    imgui.run(a, 1);
    near = probe.find(names.format(kids[0]->key), "##Popup");
    CHECK(near);
    if (near) ClickItem(imgui, a, *near);
    imgui.run(a, 1);
    CHECK(a.selection() == kids[0]);
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: undo and redo: Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z, one step per action, the Edit menu's history") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN")); // one pack: each step and undo writes or reads it
    // Undo makes a new database: look collections up again each time.
    auto rev = [&]() { return a.database()->findCollection(StringHash32("audio_revlimiter"), StringHash32("default")); };
    auto u32 = [&](const char* f) { return load32(rev()->find(StringHash32(f))->data.bytes.data(), Endian::Little); };
    const uint32_t shortWas = u32("total_frames_short"), longWas = u32("total_frames_long");
    CHECK(shortWas != 123u && longWas != 77u);
    a.select(rev(), true);
    imgui.run(a, 2);
    CHECK_EQ(a.undoLabels().size(), size_t(1)); // the opening
    CHECK(probe.find("default", "##classes"));

    // Typing a value is one step, however many keys.
    const test::ProbeItem* filter = probe.find("##fieldfilter", "Collection");
    CHECK(filter);
    ClickItem(imgui, a, *filter);
    imgui.type(a, "total_frames_short");
    imgui.run(a, 2);
    const test::ProbeItem* field = OnRow(probe, "total_frames_short", "##int");
    CHECK(field);
    if (!field) return;
    ClickItem(imgui, a, *field);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
    imgui.type(a, "123");
    imgui.run(a, 1);
    CHECK_EQ(a.undoLabels().size(), size_t(1)); // still typing
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    CHECK_EQ(u32("total_frames_short"), 123u);
    CHECK_EQ(a.undoLabels().size(), size_t(2));
    CHECK(a.undoLabels()[1].find("total_frames_short") != std::string::npos);

    CHECK(a.runConsole("edit audio_revlimiter/default { total_frames_long = 77 }"));
    imgui.run(a, 1);
    CHECK_EQ(a.undoLabels().size(), size_t(3));
    CHECK(a.undoLabels()[2].rfind("Console:", 0) == 0);

    // Ctrl+Z steps back; the tab and the open tree follow into the new database.
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_Z);
    imgui.run(a, 1);
    CHECK_EQ(u32("total_frames_long"), longWas);
    CHECK_EQ(u32("total_frames_short"), 123u);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_Z);
    imgui.run(a, 2);
    CHECK_EQ(u32("total_frames_short"), shortWas);
    CHECK_EQ(a.undoIndex(), 0);
    CHECK(!a.hasUnsavedChanges()); // back to what the files hold
    CHECK(a.selection() == rev());
    CHECK(probe.find("default", "##classes"));
    // Ctrl+Y and Ctrl+Shift+Z redo.
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_Y);
    imgui.run(a, 1);
    CHECK_EQ(u32("total_frames_short"), 123u);
    CHECK(a.hasUnsavedChanges());
    imgui.key(a, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z);
    imgui.run(a, 1);
    CHECK_EQ(u32("total_frames_long"), 77u);

    // A deleted collection comes back, and the tab on it returns.
    auto [parent, kids] = ParentWithLeaves(*a.database());
    CHECK(parent);
    const std::pair<uint32_t, uint32_t> leaf{kids[0]->classKey, kids[0]->key};
    a.select(kids[0], true);
    imgui.run(a, 1);
    CHECK(a.removeTree(kids[0]));
    imgui.run(a, 1);
    CHECK(!a.database()->findCollection(leaf.first, leaf.second));
    CHECK(a.undoLabels().back().rfind("Delete", 0) == 0);
    CHECK(a.undo());
    imgui.run(a, 1);
    Collection* back = a.database()->findCollection(leaf.first, leaf.second);
    CHECK(back);
    CHECK(a.selection() == back);
    // A new action after an undo drops what was undone.
    CHECK(a.runConsole("edit audio_revlimiter/default { total_frames_long = 78 }"));
    imgui.run(a, 1);
    CHECK(!a.redo());
    CHECK_EQ(a.undoLabels().size(), size_t(4));

    // The Edit menu names the step and lists the history.
    const test::ProbeItem* edit = probe.find("Edit");
    CHECK(edit);
    ClickItem(imgui, a, *edit);
    imgui.run(a, 1);
    CHECK(probe.find("Undo Console: edit audio_revlimiter/default { total_frames_long = 78 }"));
    CHECK(probe.find("Undo History"));
    imgui.key(a, ImGuiKey_Escape);
    imgui.run(a, 1);
    CHECK(a.goToUndo(0));
    imgui.run(a, 1);
    CHECK_EQ(u32("total_frames_short"), shortWas);
    CHECK_EQ(u32("total_frames_long"), longWas);

    // Only the last kUndoSteps are kept.
    for (int i = 0; i < app::App::kUndoSteps + 5; ++i) {
        store32(rev()->find(StringHash32("total_frames_long"))->data.bytes.data(), uint32_t(100 + i), Endian::Little);
        a.markDirty(rev());
        imgui.run(a, 1);
    }
    CHECK_EQ(a.undoLabels().size(), size_t(app::App::kUndoSteps + 1));
    CHECK(a.goToUndo(0));
    CHECK_EQ(u32("total_frames_long"), 104u); // the oldest kept: after the fifth edit
    CHECK(!a.undo());
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: code editors undo their own text (Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z) and have a right-click menu") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    CHECK(a.runConsole("edit audio_revlimiter/default { total_frames_short = 9 }"));
    imgui.run(a, 1);
    CHECK_EQ(a.undoLabels().size(), size_t(2));
    const std::string original = "edit audio_revlimiter/default {\n}\n";
    a.setScriptText(original);
    imgui.run(a, 2);
    const test::ProbeItem* ed = probe.find("##script", "Script");
    CHECK(ed);
    if (!ed) return;
    imgui.click(a, ed->rect.Min.x + 40, ed->rect.Min.y + 8);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_End);
    imgui.type(a, "# one");
    imgui.run(a, 1);
    const std::string typed = a.scriptText();
    CHECK(typed == original + "# one");

    // Ctrl+Z while typing is the editor's: the database keeps its step.
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_Z);
    imgui.run(a, 2);
    CHECK(a.scriptText() == original);
    CHECK_EQ(a.undoIndex(), 1);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_Y);
    imgui.run(a, 2);
    CHECK(a.scriptText() == typed);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_Z);
    imgui.run(a, 2);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z);
    imgui.run(a, 2);
    CHECK(a.scriptText() == typed);

    // The menu: Select All, Cut, Paste, Undo.
    auto menu = [&](const char* item) {
        Rightclick(imgui, a, probe.find("##script", "Script"));
        const test::ProbeItem* it = probe.find(item, "##Popup");
        CHECK(it);
        if (it) ClickItem(imgui, a, *it);
        imgui.run(a, 2);
    };
    menu("Select All");
    menu("Cut");
    CHECK(a.scriptText().empty());
    CHECK(Clipboard() == typed);
    menu("Paste");
    CHECK(a.scriptText() == typed);
    menu("Undo");
    CHECK(a.scriptText().empty());
    menu("Undo");
    CHECK(a.scriptText() == typed);
    menu("Redo");
    CHECK(a.scriptText().empty());
    CHECK_EQ(a.undoIndex(), 1); // none of it touched the database's history

    // The console line has a menu too: paste a statement, run it with Enter.
    ImGui::SetClipboardText("edit audio_revlimiter/default { total_frames_short = 5 }");
    const test::ProbeItem* console = probe.find("##console", "Script");
    CHECK(console);
    if (!console) return;
    Rightclick(imgui, a, *console);
    const test::ProbeItem* paste = probe.find("Paste", "##Popup");
    CHECK(paste);
    if (paste) ClickItem(imgui, a, *paste);
    imgui.run(a, 1);
    ClickItem(imgui, a, probe.find("##console", "Script"));
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    Collection* rev = a.database()->findCollection(StringHash32("audio_revlimiter"), StringHash32("default"));
    CHECK_EQ(load32(rev->find(StringHash32("total_frames_short"))->data.bytes.data(), Endian::Little), 5u);
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: the unsaved changes prompt reads Save All, Cancel, Discard, and Enter cancels") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    CHECK(a.runConsole("edit audio_revlimiter/default { total_frames_short = 9 }"));
    imgui.run(a, 1);
    CHECK(!a.requestClose());
    imgui.run(a, 2);
    const test::ProbeItem* save = probe.find("Save All", "Unsaved Changes");
    const test::ProbeItem* cancel = probe.find("Cancel", "Unsaved Changes");
    const test::ProbeItem* discard = probe.find("Discard", "Unsaved Changes");
    CHECK(save && cancel && discard);
    if (!save || !cancel || !discard) return;
    CHECK(save->rect.Max.x < cancel->rect.Min.x && cancel->rect.Max.x < discard->rect.Min.x);
    // The keyboard starts on Cancel: Enter neither saves nor discards.
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    CHECK(!probe.find("Save All", "Unsaved Changes"));
    CHECK(!host.quitRequested);
    CHECK(a.hasUnsavedChanges());
}

TEST("ui: an array's count: typing applies on Enter; shrinking and growing again brings the elements back") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    // An entry array with distinct elements.
    CHECK(a.runConsole("edit collisionsfx_damage/hood { VolMap = [5, 6, 7, 8, 9] }"));
    auto vol = [&]() { return a.database()->findCollection(StringHash32("collisionsfx_damage"), StringHash32("hood"))->find(StringHash32("VolMap")); };
    a.select(a.database()->findCollection(StringHash32("collisionsfx_damage"), StringHash32("hood")), true);
    imgui.run(a, 2);
    const test::ProbeItem* filter = probe.find("##fieldfilter", "Collection");
    CHECK(filter);
    ClickItem(imgui, a, *filter);
    imgui.type(a, "VolMap");
    imgui.run(a, 2);
    // Typing 2 over 5: nothing changes until Enter.
    const test::ProbeItem* count = OnRow(probe, "VolMap", "##count");
    CHECK(count);
    if (!count) return;
    ClickItem(imgui, a, *count);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
    imgui.type(a, "2");
    imgui.run(a, 1);
    CHECK_EQ(vol()->count, 5u);
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    CHECK_EQ(vol()->count, 2u);
    // + brings back the elements the shrink took, in order.
    for (int i = 0; i < 3; ++i) {
        const test::ProbeItem* plus = OnRow(probe, "VolMap", "+");
        CHECK(plus);
        if (plus) ClickItem(imgui, a, *plus);
        imgui.run(a, 1);
    }
    CHECK_EQ(vol()->count, 5u);
    const uint32_t es = vol()->elemSize;
    for (uint32_t i = 0; i < 5; ++i) CHECK_EQ(int(load32(vol()->data.bytes.data() + i * es, Endian::Little)), int(5 + i));
    // One more is new: zero.
    ClickItem(imgui, a, *OnRow(probe, "VolMap", "+"));
    imgui.run(a, 1);
    CHECK_EQ(vol()->count, 6u);
    CHECK_EQ(load32(vol()->data.bytes.data() + 5 * es, Endian::Little), 0u);
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: an array stops at its definition's count until Edit > Allow Arrays Past Their Limit is on; then it warns") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(!a.arraysPastLimit()); // off by default
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    // vehicle's GEAR_RATIO: 8 elements, and its definition allows 9.
    auto gear = [&]() { return a.database()->findCollection(StringHash32("vehicle"), StringHash32("997gt3rs"))->find(StringHash32("GEAR_RATIO")); };
    a.select(a.database()->findCollection(StringHash32("vehicle"), StringHash32("997gt3rs")), true);
    imgui.run(a, 2);
    ClickItem(imgui, a, probe.find("##fieldfilter", "Collection"));
    imgui.type(a, "GEAR_RATIO");
    imgui.run(a, 2);
    auto typeCount = [&](const char* n) {
        const test::ProbeItem* count = OnRow(probe, "GEAR_RATIO", "##count");
        CHECK(count);
        if (!count) return;
        ClickItem(imgui, a, *count);
        imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
        imgui.type(a, n);
        imgui.run(a, 1);
        imgui.key(a, ImGuiKey_Enter);
        imgui.run(a, 2);
    };
    auto plus = [&]() {
        const test::ProbeItem* b = OnRow(probe, "GEAR_RATIO", "+");
        CHECK(b);
        if (b) ClickItem(imgui, a, *b);
        imgui.run(a, 1);
    };
    auto limitWarnings = [&](size_t from) {
        size_t n = 0;
        for (size_t i = from; i < a.log().size(); ++i)
            n += a.log()[i].level == app::LogLevel::Warning && a.log()[i].text.find("more than the 9 its definition allows") != std::string::npos;
        return n;
    };
    // Off: a typed 12 stops at 9, and + does nothing there.
    typeCount("12");
    CHECK_EQ(gear()->count, 9u);
    plus();
    CHECK_EQ(gear()->count, 9u);
    CHECK_EQ(limitWarnings(0), size_t(0));

    // The Edit menu turns it on.
    const test::ProbeItem* edit = probe.find("Edit");
    CHECK(edit);
    if (edit) ClickItem(imgui, a, *edit);
    imgui.run(a, 1);
    const test::ProbeItem* item = probe.find("Allow Arrays Past Their Limit");
    CHECK(item);
    if (item) ClickItem(imgui, a, *item);
    imgui.run(a, 2);
    CHECK(a.arraysPastLimit());

    // On: the array goes past, with one warning as it does.
    const size_t before = a.log().size();
    typeCount("12");
    CHECK_EQ(gear()->count, 12u);
    CHECK_EQ(limitWarnings(before), size_t(1));
    plus();
    CHECK_EQ(gear()->count, 13u);
    CHECK_EQ(limitWarnings(before), size_t(1));
    // Back at the limit and over it again warns again.
    typeCount("9");
    CHECK_EQ(gear()->count, 9u);
    plus();
    CHECK_EQ(gear()->count, 10u);
    CHECK_EQ(limitWarnings(before), size_t(2));
    // Turned off, an array already past it keeps its elements and can only shrink.
    a.setArraysPastLimit(false);
    plus();
    CHECK_EQ(gear()->count, 10u);
    typeCount("11");
    CHECK_EQ(gear()->count, 10u);
    typeCount("8");
    CHECK_EQ(gear()->count, 8u);
    // Back to its count, it holds no spare capacity (saving writes it as read).
    CHECK_EQ(uint32_t(gear()->capacity), 8u);
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: with the option off, an array a script took past its limit keeps its elements: drawn, edited, undone and saved") {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "attribulina_past_limit_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    fs::copy_file(fs::path(ProStreetDir()) / "ATTRIBUTES.BIN", tmp / "ATTRIBUTES.BIN");
    const std::string file = (tmp / "ATTRIBUTES.BIN").string();
    const std::vector<float> ratios = {3.446f, 0, 3.82f, 2.14f, 1.48f, 1.18f, 0.97f, 0.775f, 0.66f, 0.58f, 0.5f};
    auto same = [&](const Attribute* g) {
        bool ok = g && g->count == ratios.size();
        for (uint32_t i = 0; ok && i < g->count; ++i) ok = loadf32(g->elem(i), Endian::Little) == ratios[i];
        return ok;
    };
    {
        platform::HeadlessImGui imgui;
        imgui.beforeFrame = ProbeBefore;
        imgui.afterFrame = ProbeAfter;
        test::UiProbe& probe = test::UiProbe::get();
        platform::HeadlessHost host;
        app::App a(host);
        a.setup(1.0f, "");
        a.setUserKeysPath("");
        CHECK(!a.arraysPastLimit());
        CHECK(a.open(file));
        CHECK(a.runConsole("edit vehicle/997gt3rs { GEAR_RATIO = [3.446, 0, 3.82, 2.14, 1.48, 1.18, 0.97, 0.775, 0.66, 0.58, 0.5] }"));
        auto gear = [&]() { return a.database()->findCollection(StringHash32("vehicle"), StringHash32("997gt3rs"))->find(StringHash32("GEAR_RATIO")); };
        CHECK(same(gear()));
        a.select(a.database()->findCollection(StringHash32("vehicle"), StringHash32("997gt3rs")), true);
        imgui.run(a, 2);
        ClickItem(imgui, a, probe.find("##fieldfilter", "Collection"));
        imgui.type(a, "GEAR_RATIO");
        imgui.run(a, 2);
        // Drawn, + (off at the limit) and a typed larger count leave it as it is.
        const test::ProbeItem* plus = OnRow(probe, "GEAR_RATIO", "+");
        CHECK(plus);
        if (plus) ClickItem(imgui, a, *plus);
        imgui.run(a, 1);
        const test::ProbeItem* count = OnRow(probe, "GEAR_RATIO", "##count");
        CHECK(count);
        if (count) {
            ClickItem(imgui, a, *count);
            imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
            imgui.type(a, "20");
            imgui.run(a, 1);
            imgui.key(a, ImGuiKey_Enter);
            imgui.run(a, 2);
        }
        CHECK(same(gear()));
        // Another edit to the collection, undone and redone.
        CHECK(a.runConsole("edit vehicle/997gt3rs { FINAL_GEAR = 3.5 }"));
        imgui.run(a, 2);
        CHECK(a.undo());
        imgui.run(a, 2);
        CHECK(same(gear()));
        CHECK(a.undo());
        imgui.run(a, 2);
        CHECK_EQ(gear()->count, 8u);
        CHECK(a.redo());
        imgui.run(a, 2);
        CHECK(same(gear()));
        CHECK(a.saveAll());
        CHECK_EQ(Errors(a), size_t(0));
    }
    {
        // Opened again with the option still off, all eleven are there.
        platform::HeadlessImGui imgui;
        platform::HeadlessHost host;
        app::App a(host);
        a.setup(1.0f, "");
        CHECK(a.open(file));
        imgui.run(a, 2);
        CHECK(same(a.database()->findCollection(StringHash32("vehicle"), StringHash32("997gt3rs"))->find(StringHash32("GEAR_RATIO"))));
        for (const auto& p : a.database()->packs())
            for (const auto& v : p->vaults) CHECK(v->warnings.empty());
    }
    fs::remove_all(tmp);
}

TEST("ui: a field's menu writes it as nfsms update_field lines") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    CHECK(a.runConsole("edit collisionsfx_damage/hood { VolMap = [5, 6, 7] }"));
    a.select(a.database()->findCollection(StringHash32("collisionsfx_damage"), StringHash32("hood")), true);
    imgui.run(a, 2);
    ClickItem(imgui, a, probe.find("##fieldfilter", "Collection"));
    imgui.type(a, "VolMap");
    imgui.run(a, 2);
    // The whole field: one line per element, nothing else.
    const test::ProbeItem* row = probe.find("VolMap", "Collection");
    CHECK(row);
    if (!row) return;
    Rightclick(imgui, a, *row);
    const test::ProbeItem* copy = probe.find("Copy as nfsms", "##Popup");
    CHECK(copy);
    if (copy) ClickItem(imgui, a, *copy);
    CHECK_EQ(Clipboard(), std::string("update_field collisionsfx_damage hood VolMap[0] 5\n"
                                      "update_field collisionsfx_damage hood VolMap[1] 6\n"
                                      "update_field collisionsfx_damage hood VolMap[2] 7\n"));
    // One element, from its own row; Add puts it in the nfsms window.
    if (const test::ProbeItem* open = probe.find("VolMap", "Collection")) ClickItem(imgui, a, *open); // expand it
    imgui.run(a, 2);
    const test::ProbeItem* el = probe.find("[1]", "Collection");
    CHECK(el);
    if (!el) return;
    Rightclick(imgui, a, *el);
    const test::ProbeItem* copyEl = probe.find("Copy Element as nfsms", "##Popup");
    CHECK(copyEl);
    if (copyEl) ClickItem(imgui, a, *copyEl);
    CHECK_EQ(Clipboard(), std::string("update_field collisionsfx_damage hood VolMap[1] 6\n"));
    Rightclick(imgui, a, probe.find("[1]", "Collection"));
    ClickItem(imgui, a, probe.find("Add Element to nfsms Script", "##Popup"));
    imgui.run(a, 1);
    CHECK_EQ(a.nfsmsText(), std::string("update_field collisionsfx_damage hood VolMap[1] 6\n"));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: a gameplay.bin that isn't gameplay.lzc's pack is flagged, and Save All writes both") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "attribulina_twin_ui_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::copy_file(fs::path(ProStreetDir()) / "ATTRIBUTES.BIN", dir / "ATTRIBUTES.BIN");
    // Another gameplay.lzc beside the retail gameplay.bin: the two disagree.
    fs::copy_file(test::DataPath("attrib/databases/pc/prostreet-vlted/gameplay.lzc"), dir / "gameplay.lzc");
    fs::copy_file(fs::path(ProStreetDir()) / "gameplay.bin", dir / "gameplay.bin");

    platform::HeadlessImGui imgui;
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    CHECK(a.open(dir.string()));
    imgui.run(a, 2);
    bool flagged = false;
    for (const auto& l : a.log()) flagged |= l.level == app::LogLevel::Warning && l.text.find("isn't the same pack") != std::string::npos;
    CHECK(flagged);
    CHECK(a.hasUnsavedChanges());
    CHECK(a.saveAll());
    CHECK(!a.hasUnsavedChanges());
    CHECK(Decompress(ReadFile((dir / "gameplay.lzc").string())) == ReadFile((dir / "gameplay.bin").string()));
    CHECK(fs::exists(dir / "gameplay.bin.bak"));
    CHECK(fs::exists(dir / "gameplay.lzc.bak"));
    CHECK_EQ(Errors(a), size_t(0));
    fs::remove_all(dir);
}

TEST("ui: search: References on a collection, then Ctrl+Shift+F; a hit opens its collection") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    const uint32_t gp = StringHash32("gameplay");
    Collection* race = db->findCollection(gp, StringHash32("prostreet_career/bins/race_bin_an_2_1/gr_1"));
    Collection* layout = db->findCollection(gp, StringHash32("tracks/autobahn_gd/autobahn_gd_grip_1"));
    CHECK(race && layout);
    a.select(layout, true);
    imgui.run(a, 2);

    // The header's References button lists the race that runs on this layout.
    const test::ProbeItem* refs = probe.find("References", "Collection");
    CHECK(refs);
    ClickItem(imgui, a, *refs);
    imgui.run(a, 2);
    const std::string raceLabel = "gameplay/prostreet_career/bins/race_bin_an_2_1/gr_1";
    const test::ProbeItem* hit = probe.find(raceLabel, "Search");
    CHECK(hit);
    ClickItem(imgui, a, *hit);
    imgui.run(a, 2);
    CHECK(a.selection() == race);

    // Ctrl+Shift+F focuses the query with its text selected; typing and
    // Enter search values.
    imgui.key(a, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F);
    imgui.run(a, 1);
    CHECK(ImGui::GetIO().WantTextInput);
    imgui.type(a, "tracks/autobahn_gd/autobahn_gd_grip_1");
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    CHECK(probe.find(raceLabel, "Search"));
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: script: Apply asks for the options, tries it on a copy, then applies it") {
    namespace sc = attrib::script;
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath(""); // names it creates stay out of the repo
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    Database* db = a.database();
    Collection* rev = db->findCollection(StringHash32("audio_revlimiter"), StringHash32("default"));
    CHECK(rev);
    auto frames = [&]() { return load32(rev->find(StringHash32("total_frames_short"))->data.bytes.data(), Endian::Little); };
    const uint32_t before = frames();

    a.setScriptText("attribulina 1\n"
                    "title \"UI test\"\n"
                    "option frames integer \"Frames\" default 5 min 1 max 50\n"
                    "edit audio_revlimiter/default { total_frames_short = $frames }\n");
    imgui.run(a, 2);
    FrontTab(imgui, a, "Script");
    const test::ProbeItem* apply = probe.find("Apply...", "Script");
    CHECK(apply);
    ClickItem(imgui, a, *apply);
    imgui.run(a, 2);
    CHECK(probe.find("Frames", "Apply Script")); // the option's control
    CHECK_EQ(frames(), before);                  // nothing yet
    const test::ProbeItem* go = probe.find("Apply", "Apply Script");
    CHECK(go);
    ClickItem(imgui, a, *go);
    imgui.run(a, 2);
    CHECK_EQ(frames(), 5u);
    CHECK(db->packs().front()->dirty);
    CHECK(!probe.find("Apply", "Apply Script")); // closed

    // A script that stops halfway changes nothing, and the prompt says why.
    a.setScriptText("attribulina 1\nedit audio_revlimiter/default { total_frames_short = 42 }\nedit pvehicle/nope { HornType = 1 }\n");
    imgui.run(a, 1);
    ClickItem(imgui, a, probe.find("Apply...", "Script"));
    imgui.run(a, 2);
    ClickItem(imgui, a, probe.find("Apply", "Apply Script"));
    imgui.run(a, 2);
    CHECK_EQ(frames(), 5u);
    CHECK(probe.find("Cancel", "Apply Script")); // still open, with the error
    ClickItem(imgui, a, probe.find("Cancel", "Apply Script"));
    imgui.run(a, 2);

    // The console line applies one statement as it is typed.
    const test::ProbeItem* console = probe.find("##console", "Script");
    CHECK(console);
    ClickItem(imgui, a, *console);
    imgui.type(a, "edit audio_revlimiter/default { total_frames_short = 7 }");
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    CHECK_EQ(frames(), 7u);
    CHECK(a.runConsole("echo \"frames:\" 7"));
    CHECK(!a.runConsole("edit pvehicle/nope { HornType = 1 }"));

    // Deleting the collection a tab shows moves the tab to its parent.
    Collection* street = db->findCollection(StringHash32("pvehicle"), StringHash32("street"));
    CHECK(a.applyScript(sc::Parse("attribulina 1\ncollection pvehicle/ui_temp under pvehicle/street\n"), {}, false).ok);
    Collection* temp = db->findCollection(StringHash32("pvehicle"), StringHash32("ui_temp"));
    CHECK(temp && temp->parent == street);
    a.select(temp, true);
    imgui.run(a, 2);
    CHECK(a.applyScript(sc::Parse("attribulina 1\ndelete pvehicle/ui_temp\n"), {}, false).ok);
    CHECK(a.selection() == street);
    imgui.run(a, 2);
    CHECK_EQ(Errors(a), size_t(2)); // the stopped script's and the console's
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: script: includes start from the panel script's folder; their options are asked for") {
    namespace fs = std::filesystem;
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    Collection* rev = a.database()->findCollection(StringHash32("audio_revlimiter"), StringHash32("default"));
    CHECK(rev);
    auto frames = [&]() { return load32(rev->find(StringHash32("total_frames_short"))->data.bytes.data(), Endian::Little); };

    const fs::path dir = fs::temp_directory_path() / "attribulina_include_ui";
    fs::remove_all(dir);
    fs::create_directories(dir / "parts");
    std::ofstream(dir / "parts" / "opt.amod", std::ios::binary) << "option loud integer \"Loudness\" default 9\n"
                                                                     "edit audio_revlimiter/default { total_frames_short = $loud }\n";
    std::ofstream(dir / "parts" / "bad.amod", std::ios::binary) << "attribulina 1\necho error \"stop here\"\n";
    const std::string path = (dir / "mod.amod").string(); // the panel's file; it needn't be saved for this

    a.setScriptText("attribulina 1\ninclude parts/opt.amod\n", path);
    imgui.run(a, 2);
    FrontTab(imgui, a, "Script");
    ClickItem(imgui, a, probe.find("Apply...", "Script"));
    imgui.run(a, 2);
    CHECK(probe.find("Loudness", "Apply Script")); // the included script's option
    ClickItem(imgui, a, probe.find("Apply", "Apply Script"));
    imgui.run(a, 2);
    CHECK_EQ(frames(), 9u);

    // Check marks the include line, and the message names the included line.
    a.setScriptText("attribulina 1\n# two\ninclude parts/bad.amod\n", path);
    imgui.run(a, 2);
    ClickItem(imgui, a, probe.find("Check", "Script"));
    imgui.run(a, 2);
    CHECK(probe.find("line:3: in " + (fs::path("parts") / "bad.amod").string() + ":2: stop here", "Script"));

    // A script that isn't saved has no folder for relative paths.
    a.setScriptText("attribulina 1\ninclude parts/opt.amod\n");
    imgui.run(a, 2);
    ClickItem(imgui, a, probe.find("Check", "Script"));
    imgui.run(a, 2);
    CHECK(probe.find("line:2: include \"parts/opt.amod\": a script that isn't saved has no folder for a relative path; save it first, or give the full path",
                     "Script"));
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
    fs::remove_all(dir);
}

TEST("ui: script: Add to Script and Export Changes fill the Script panel") {
    namespace sc = attrib::script;
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath(""); // names it creates stay out of the repo
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    Database* db = a.database();
    Collection* street = db->findCollection(StringHash32("pvehicle"), StringHash32("street"));
    a.select(street, true);
    imgui.run(a, 3);

    // The collection's menu: Add to Script > This Collection.
    const test::ProbeItem* node = probe.find("street", "##classes");
    CHECK(node);
    Rightclick(imgui, a, *node);
    const test::ProbeItem* sub = probe.find("Add to Script", "##Popup");
    CHECK(sub);
    ClickItem(imgui, a, *sub);
    imgui.run(a, 1);
    const test::ProbeItem* item = probe.find("This Collection");
    CHECK(item);
    ClickItem(imgui, a, *item);
    imgui.run(a, 2);
    CHECK(a.scriptText().find("collection pvehicle/street under ") != std::string::npos);
    CHECK(a.scriptText().find("    HornType = 5\n") != std::string::npos);

    // A field's value, twice: one edit block, one line.
    const uint32_t horn = StringHash32("HornType");
    a.setScriptText({});
    CHECK(a.addToScript({sc::ExportValue(*db, *street, horn)}));
    CHECK(a.addToScript({sc::ExportValue(*db, *street, horn)}));
    CHECK_EQ(a.scriptText(), std::string("attribulina 1\ngame prostreet_pc\nedit pvehicle/street {\n    HornType = 5\n}\n"));

    // What changed since the file was saved, with the old values.
    CHECK(a.applyScript(sc::Parse("attribulina 1\nedit audio_revlimiter/default { total_frames_short = 9 }\n"), {}, false).ok);
    a.setScriptText({});
    imgui.run(a, 1);
    ClickItem(imgui, a, probe.find("File"));
    ClickItem(imgui, a, probe.find("Export Changes as Script..."));
    imgui.run(a, 2);
    const test::ProbeItem* add = probe.find("Add to Script", "Export Changes");
    CHECK(add);
    ClickItem(imgui, a, *add);
    imgui.run(a, 2);
    CHECK(a.scriptText().find("edit audio_revlimiter/default {\n    total_frames_short = 9 was 3\n}\n") != std::string::npos);
    // A script that doesn't read isn't added to.
    a.setScriptText("edit pvehicle {\n");
    CHECK(!a.addToScript({sc::ExportValue(*db, *street, horn)}));
    CHECK_EQ(a.scriptText(), std::string("edit pvehicle {\n"));
    imgui.run(a, 2);
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: script: a gameplay folder and a vault go to the script from their menus, with the vaults they need") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir()));
    Database* db = a.database();
    imgui.run(a, 3);
    auto has = [&](const std::string& text) {
        const bool there = a.scriptText().find(text) != std::string::npos;
        if (!there) std::printf("        not in the script: %s\n", text.c_str());
        return there;
    };
    // Right-click `label` in `window`, then Add to Script > `item`.
    auto addToScript = [&](const std::string& label, const char* window, const char* item) {
        Rightclick(imgui, a, probe.find(label, window));
        const test::ProbeItem* sub = probe.find("Add to Script", "##Popup");
        CHECK(sub);
        if (!sub) return;
        ClickItem(imgui, a, *sub);
        imgui.run(a, 1);
        const test::ProbeItem* it = probe.find(item);
        CHECK(it);
        if (it) ClickItem(imgui, a, *it);
        imgui.run(a, 2);
    };

    // A path no element has: its race, and the checkpoints the race lists under
    // a track's path. (Revealing the race scrolls the tree to the folder.)
    Collection* shortcut = db->findCollection(StringHash32("gameplay"), StringHash32("race_bin_exotic/4_5_2/shortcut06"));
    CHECK(shortcut);
    a.select(shortcut, true);
    imgui.run(a, 3);
    addToScript("race_bin_exotic", "##gameplay", "With Everything Under It");
    CHECK(has("collection gameplay/race_bin_exotic/4_5_2/shortcut06 under "));
    CHECK(has("collection gameplay/tracks/leipzig/leipzig_grip_1_gp/checkpoint1 under gameplay/checkpoint in vault "));

    // A race bin: nothing is made from it as a template, and everything under it
    // comes with the vaults it is kept in and the Children list naming it.
    a.setScriptText({});
    Collection* bin = db->findCollection(StringHash32("gameplay"), StringHash32("prostreet_career/bins/race_bin_an_2_1"));
    CHECK(bin && bin->children.empty());
    a.select(bin, true);
    imgui.run(a, 3);
    addToScript("race_bin_an_2_1", "##gameplay", "With Everything Under It");
    CHECK(has("create vault race_bin_an_2_1 in gameplay.lzc\ncreate vault an_2_1_ss_1 in gameplay.lzc\n"));
    CHECK(has("collection gameplay/prostreet_career/bins/race_bin_an_2_1/ss_1/sector1 under gameplay/sectortrigger in vault an_2_1_ss_1 {"));
    CHECK(has("edit gameplay/prostreet_career/bins {\n    Children = ["));

    // A race with nothing under it: itself, its vault and its bin's list.
    a.setScriptText({});
    Collection* race = db->findCollection(StringHash32("gameplay"), StringHash32("prostreet_career/bins/race_bin_an_2_1/gr_1"));
    CHECK(race);
    a.select(race, true);
    imgui.run(a, 3);
    addToScript("gr_1", "##gameplay", "With Everything Under It");
    CHECK(has("create vault race_bin_an_2_1 in gameplay.lzc\n"
              "collection gameplay/prostreet_career/bins/race_bin_an_2_1/gr_1 under gameplay/circuit in vault race_bin_an_2_1 {"));
    CHECK(has("edit gameplay/prostreet_career/bins/race_bin_an_2_1 {\n    Children = ["));

    // The Files tab: a vault's menu.
    a.setScriptText({});
    ClickItem(imgui, a, probe.find("Files", "Explorer"));
    imgui.run(a, 2);
    const LoadedPack* gp = nullptr;
    for (const auto& pk : db->packs())
        if (std::filesystem::path(pk->path).filename() == "gameplay.lzc") gp = pk.get();
    CHECK(gp && gp->vaults.front()->name == "an_2_1_ss_1");
    if (!gp) return;
    ClickItem(imgui, a, probe.find("gameplay.lzc  (" + std::to_string(gp->vaults.size()) + " vaults)", "##files"));
    imgui.run(a, 2);
    const std::string vaultLabel = "an_2_1_ss_1  (" + std::to_string(gp->vaults.front()->collections.size()) + ")";
    addToScript(vaultLabel, "##files", "With Its Collections");
    // In the vault's order.
    CHECK(has("create vault an_2_1_ss_1 in gameplay.lzc\ncollection gameplay/prostreet_career/bins/race_bin_an_2_1/ss_1/sector3 under "));
    CHECK(has("collection gameplay/prostreet_career/bins/race_bin_an_2_1/ss_1/sector1 under gameplay/sectortrigger in vault an_2_1_ss_1 {"));
    a.setScriptText({});
    addToScript(vaultLabel, "##files", "This Vault");
    CHECK_EQ(a.scriptText(), std::string("attribulina 1\ngame prostreet_pc\ncreate vault an_2_1_ss_1 in gameplay.lzc\n"));
    // Added twice, it is there once.
    addToScript(vaultLabel, "##files", "This Vault");
    CHECK_EQ(a.scriptText(), std::string("attribulina 1\ngame prostreet_pc\ncreate vault an_2_1_ss_1 in gameplay.lzc\n"));

    CHECK_EQ(Errors(a), size_t(0));
    CHECK(!a.hasUnsavedChanges());
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: the code editor draws its coloured text where InputText draws text") {
    platform::HeadlessImGui imgui;
    ImGui::GetIO().IniFilename = nullptr; // no App to set it: keep ImGui from writing imgui.ini here
    std::string text = "edit pvehicle/street {\n\tHornType = 3  # a comment\n}\n";
    // One frame with `body` in a fixed window; then the bounds of the glyphs
    // the editor's child window holds.
    auto frame = [&](const std::function<void()>& body) {
        ImGui_ImplNullPlatform_NewFrame();
        ImGui_ImplNullRender_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20, 30));
        ImGui::SetNextWindowSize(ImVec2(700, 400));
        ImGui::Begin("Editor test");
        body();
        ImGui::End();
        ImGui::Render();
        ImGui_ImplNullRender_RenderDrawData(ImGui::GetDrawData());
    };
    auto glyphs = [&]() {
        ImRect r(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
        size_t n = 0;
        const ImVec2 white = ImGui::GetIO().Fonts->TexUvWhitePixel;
        for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) {
            if (std::string(w->Name).find("Editor test/##code_") != 0) continue;
            for (const ImDrawVert& v : w->DrawList->VtxBuffer)
                if (v.uv.x != white.x || v.uv.y != white.y) {
                    r.Add(v.pos);
                    ++n;
                }
        }
        return std::make_pair(r, n);
    };
    app::CodeEditor ed(app::CodeLanguage::Amod);
    for (int i = 0; i < 3; ++i) frame([&]() { ed.draw("##code", text, ImVec2(600, 300)); });
    const auto [mine, mineN] = glyphs();
    CHECK(mineN > 0);
    // The same text drawn by InputText itself, after a gutter of the same width.
    const float gutterW = ImGui::CalcTextSize("99").x + ImGui::GetStyle().FramePadding.x * 2;
    for (int i = 0; i < 3; ++i)
        frame([&]() {
            ImGui::Dummy(ImVec2(gutterW, 300));
            ImGui::SameLine(0, 0);
            ImGui::InputTextMultiline("##code", &text, ImVec2(600 - gutterW, 300));
        });
    const auto [theirs, theirsN] = glyphs();
    CHECK_EQ(mineN, theirsN);
    CHECK(std::fabs(mine.Min.x - theirs.Min.x) < 0.01f && std::fabs(mine.Min.y - theirs.Min.y) < 0.01f);
    CHECK(std::fabs(mine.Max.x - theirs.Max.x) < 0.01f && std::fabs(mine.Max.y - theirs.Max.y) < 0.01f);
    // The caret line and the marks.
    ed.setMarks({{2, "bad value"}});
    ed.gotoLine(2);
    for (int i = 0; i < 3; ++i) frame([&]() { ed.draw("##code", text, ImVec2(600, 300)); });
    CHECK_EQ(ed.cursorLine(), 2);
    CHECK_EQ(ed.currentLineText(text), std::string("\tHornType = 3  # a comment"));
}

TEST("ui: code editors scroll sideways under a scrollbar; text boxes keep their selection under the right-click menu") {
    platform::HeadlessImGui imgui;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // as the App sets it: Escape closes menus
    test::UiProbe& probe = test::UiProbe::get();
    std::function<void()> body;
    auto frame = [&]() {
        ProbeBefore();
        ImGui_ImplNullPlatform_NewFrame();
        ImGui_ImplNullRender_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20, 30));
        ImGui::SetNextWindowSize(ImVec2(500, 300));
        ImGui::Begin("Scroll test");
        body();
        ImGui::End();
        ImGui::Render();
        ImGui_ImplNullRender_RenderDrawData(ImGui::GetDrawData());
        ProbeAfter();
    };
    auto frames = [&](int n) {
        for (int i = 0; i < n; ++i) frame();
    };
    auto move = [&](float x, float y) {
        io.AddMousePosEvent(x, y);
        frame();
    };
    auto button = [&](int b, bool down) {
        io.AddMouseButtonEvent(b, down);
        frame();
    };
    auto click = [&](float x, float y, int b = 0) {
        move(x, y);
        button(b, true);
        button(b, false);
    };
    auto key = [&](int chord) {
        const ImGuiKey k = ImGuiKey(chord & ~ImGuiMod_Mask_);
        if (chord & ImGuiMod_Ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
        if (chord & ImGuiMod_Shift) io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddKeyEvent(k, true);
        frame();
        io.AddKeyEvent(k, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        io.AddKeyEvent(ImGuiMod_Shift, false);
        frame();
    };
    auto window = [](const char* prefix) -> ImGuiWindow* {
        for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
            if (std::string(w->Name).find(prefix) == 0) return w;
        return nullptr;
    };
    // Rectangles in the selection colour (the editor's own, while the InputText doesn't draw one).
    auto selectionVerts = [](ImGuiWindow* w) {
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
        size_t n = 0;
        for (const ImDrawVert& v : w->DrawList->VtxBuffer) n += v.col == col;
        return n;
    };
    const ImGuiStyle& style = ImGui::GetStyle();
    float cw = 0, fh = 0; // a character's width and the line height (known in a frame)

    std::string text = "short\nlong = \"" + std::string(200, 'a') + "\"\nend\n";
    app::CodeEditor ed(app::CodeLanguage::Amod);
    body = [&]() {
        cw = ImGui::CalcTextSize("a").x;
        fh = ImGui::GetFontSize();
        ed.draw("##code", text, ImVec2(400, 200));
    };
    frames(3);
    ImGuiWindow* child = window("Scroll test/##code_");
    CHECK(child);
    if (!child) return;
    // The box leaves room under it for the scrollbar.
    CHECK_EQ(child->Size.y, 200 - style.ScrollbarSize);
    CHECK_EQ(ed.scrollX(), 0.0f);
    // Shift+wheel over the box scrolls it sideways.
    move(child->Pos.x + 100, child->Pos.y + 30);
    io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddMouseWheelEvent(0, -2);
    frame();
    io.AddKeyEvent(ImGuiMod_Shift, false);
    frame();
    const float scrolled = ed.scrollX();
    CHECK(scrolled > 0);
    // A click into the scrolled box keeps the scroll and puts the caret under the mouse.
    const float originX = child->Pos.x + style.FramePadding.x - scrolled;
    const int col = 20;
    click(originX + (col + 0.3f) * cw, child->Pos.y + style.FramePadding.y + fh * 1.5f);
    CHECK(ed.active());
    CHECK_EQ(ed.scrollX(), scrolled);
    io.AddInputCharacter('Q');
    frames(2);
    const size_t line2 = text.find('\n') + 1;
    CHECK_EQ(text[line2 + col], 'Q');
    CHECK_EQ(ed.scrollX(), scrolled);

    // Dragging the scrollbar keeps the selection; the keyboard comes back after.
    key(ImGuiMod_Shift | ImGuiKey_End);
    const std::string sel = ed.selectedText(text);
    CHECK_EQ(sel, text.substr(line2 + col + 1, text.find('\n', line2) - (line2 + col + 1))); // after the Q to the line's end
    const float barY = child->Pos.y + child->Size.y + style.ScrollbarSize * 0.5f;
    move(child->Pos.x + child->Size.x * 0.8f, barY);
    button(0, true);
    move(child->Pos.x + child->Size.x * 0.85f, barY);
    CHECK(!ed.active());
    CHECK(selectionVerts(child) > 0);
    button(0, false);
    frames(3);
    const float dragged = ed.scrollX();
    CHECK(dragged > scrolled);
    CHECK(ed.active());
    CHECK_EQ(ed.selectedText(text), sel);
    CHECK_EQ(ed.scrollX(), dragged); // not back to the caret

    // The right-click menu: the editor draws the selection while the menu has
    // the keyboard, and Escape or Copy gives it back with the same selection.
    const ImVec2 inBox(child->Pos.x + 60, child->Pos.y + 40);
    click(inBox.x, inBox.y, 1);
    frame();
    CHECK(!ed.active());
    CHECK(selectionVerts(child) > 0);
    key(ImGuiKey_Escape);
    frames(3);
    CHECK(ed.active());
    CHECK_EQ(ed.selectedText(text), sel);
    CHECK_EQ(ed.scrollX(), dragged);
    ImGui::SetClipboardText("");
    click(inBox.x, inBox.y, 1);
    frame();
    const test::ProbeItem* copy = probe.find("Copy", "##Popup");
    CHECK(copy);
    if (copy) click((copy->rect.Min.x + copy->rect.Max.x) * 0.5f, (copy->rect.Min.y + copy->rect.Max.y) * 0.5f);
    frames(3);
    CHECK_EQ(Clipboard(), sel);
    CHECK(ed.active());
    CHECK_EQ(ed.selectedText(text), sel);
    // A click elsewhere closes it and keeps the keyboard there.
    click(inBox.x, inBox.y, 1);
    frame();
    click(20 + 450, 30 + 280);
    frames(3);
    CHECK(!ed.active());

    // The log and a one-line field do the same under their menus.
    std::string log = "first line\nsecond line\n";
    std::string field = "edit pvehicle/default";
    app::LogView view;
    const std::vector<uint8_t> levels = {0, 0};
    const ImU32 colours[1] = {IM_COL32_WHITE};
    ImGuiID logId = 0, fieldId = 0;
    body = [&]() {
        logId = ImGui::GetID("##log");
        view.draw("##log", log, levels, colours, ImVec2(400, 150));
        fieldId = ImGui::GetID("##field");
        ImGui::InputText("##field", &field);
        app::TextFieldMenu(field);
    };
    frames(3);
    ImGuiWindow* logChild = window("Scroll test/##log_");
    CHECK(logChild);
    if (!logChild) return;
    auto selection = [](ImGuiID id) {
        ImGuiInputTextState* st = ImGui::GetInputTextState(id);
        return st ? std::make_pair(st->GetSelectionStart(), st->GetSelectionEnd()) : std::make_pair(-1, -1);
    };
    click(logChild->Pos.x + 30, logChild->Pos.y + 8);
    key(ImGuiMod_Ctrl | ImGuiKey_A);
    CHECK(ImGui::GetActiveID() == logId);
    click(logChild->Pos.x + 30, logChild->Pos.y + 8, 1);
    frame();
    CHECK(ImGui::GetActiveID() != logId);
    CHECK(selectionVerts(logChild) > 0);
    key(ImGuiKey_Escape);
    frames(3);
    CHECK(ImGui::GetActiveID() == logId);
    CHECK(selection(logId) == std::make_pair(0, int(log.size())));

    const ImGuiWindow* win = window("Scroll test");
    const float fieldY = logChild->Pos.y + logChild->Size.y + style.ItemSpacing.y + fh * 0.5f + style.FramePadding.y;
    click(win->Pos.x + 60, fieldY);
    CHECK(ImGui::GetActiveID() == fieldId);
    key(ImGuiMod_Ctrl | ImGuiKey_A);
    const std::string word = "pvehicle";
    ImGui::GetInputTextState(fieldId)->SetSelection(5, 5 + int(word.size()));
    frame();
    click(win->Pos.x + 60, fieldY, 1);
    frame();
    CHECK(ImGui::GetActiveID() != fieldId);
    key(ImGuiKey_Escape);
    frames(3);
    CHECK(ImGui::GetActiveID() == fieldId);
    CHECK(selection(fieldId) == std::make_pair(5, 5 + int(word.size())));
    CHECK(probe.conflicts.empty());
}

TEST("ui: the log is a read-only text box: its coloured rows wrap where ImGui wraps them, and it copies") {
    {
        platform::HeadlessImGui imgui;
        ImGui::GetIO().IniFilename = nullptr;
        std::string text = "Opened GLOBAL/ATTRIBUTES.BIN (1 vault(s), 6830 collections)\n"
                           "Click Me To Install!.nfsms:67: Main\\Changes in performance\\AI PERFORMANCEv4.2.nfsms:282: no collection "
                           "vehicle/0x33341dbf_ai for the reference vehicle[4] (set anyway)\n"
                           "\n"
                           "a_word_far_too_long_for_one_row_of_this_narrow_box_so_it_breaks_in_the_middle_of_it_somewhere\n"
                           "Saved";
        auto frame = [&](const std::function<void()>& body) {
            ImGui_ImplNullPlatform_NewFrame();
            ImGui_ImplNullRender_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(20, 30));
            ImGui::SetNextWindowSize(ImVec2(400, 500));
            ImGui::Begin("Log test");
            body();
            ImGui::End();
            ImGui::Render();
            ImGui_ImplNullRender_RenderDrawData(ImGui::GetDrawData());
        };
        auto glyphs = [&]() {
            std::vector<std::pair<float, float>> v;
            const ImVec2 white = ImGui::GetIO().Fonts->TexUvWhitePixel;
            for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) {
                if (std::string(w->Name).find("Log test/##log_") != 0) continue;
                for (const ImDrawVert& x : w->DrawList->VtxBuffer)
                    if (x.uv.x != white.x || x.uv.y != white.y) v.emplace_back(x.pos.x, x.pos.y);
            }
            std::sort(v.begin(), v.end());
            return v;
        };
        app::LogView view;
        const std::vector<uint8_t> levels = {0, 1, 0, 2, 0};
        const ImU32 colours[3] = {IM_COL32_WHITE, IM_COL32(255, 200, 0, 255), IM_COL32(255, 0, 0, 255)};
        for (int i = 0; i < 3; ++i) frame([&]() { view.draw("##log", text, levels, colours, ImVec2(300, 400)); });
        const auto mine = glyphs();
        CHECK(view.rows().size() > 5); // the long lines wrapped
        for (int i = 0; i < 3; ++i)
            frame([&]() { ImGui::InputTextMultiline("##log", &text, ImVec2(300, 400), ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap); });
        const auto theirs = glyphs();
        CHECK_EQ(mine.size(), theirs.size());
        CHECK(mine == theirs);
    }

    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    CHECK(!a.runConsole("edit pvehicle/nope { HornType = 1 }"));
    imgui.run(a, 2);
    FrontTab(imgui, a, "Log");
    const test::ProbeItem* box = probe.find("##log", "Log");
    CHECK(box);
    if (!box) return;
    ImGui::SetClipboardText("");
    Rightclick(imgui, a, *box);
    const test::ProbeItem* all = probe.find("Copy All", "##Popup");
    CHECK(all);
    if (all) ClickItem(imgui, a, *all);
    CHECK(Clipboard().find("Opened") != std::string::npos);
    CHECK(!Clipboard().empty() && Clipboard().back() == '\n'); // the empty line the caret waits on
    CHECK(Clipboard().find("nope") != std::string::npos);
    // Select All, then Copy, from the menu.
    ImGui::SetClipboardText("");
    Rightclick(imgui, a, probe.find("##log", "Log"));
    ClickItem(imgui, a, probe.find("Select All", "##Popup"));
    imgui.run(a, 2);
    Rightclick(imgui, a, probe.find("##log", "Log"));
    const test::ProbeItem* copy = probe.find("Copy", "##Popup");
    CHECK(copy);
    if (copy) ClickItem(imgui, a, *copy);
    CHECK(Clipboard().find("nope") != std::string::npos);
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: the log's caret waits on an empty last line and new lines take it and the view along; put elsewhere, it stays") {
    platform::HeadlessImGui imgui;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    std::string text;
    auto add = [&](int from, int n) {
        for (int i = from; i < from + n; ++i) text += "line " + std::to_string(i) + "\n";
    };
    add(0, 30);
    app::LogView view;
    const std::vector<uint8_t> levels;
    const ImU32 colours[1] = {IM_COL32_WHITE};
    ImGuiID logId = 0;
    float fh = 0;
    auto frame = [&]() {
        ImGui_ImplNullPlatform_NewFrame();
        ImGui_ImplNullRender_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20, 30));
        ImGui::SetNextWindowSize(ImVec2(500, 300));
        ImGui::Begin("Follow test");
        fh = ImGui::GetFontSize();
        logId = ImGui::GetID("##log");
        view.draw("##log", text, levels, colours, ImVec2(400, 150));
        ImGui::End();
        ImGui::Render();
        ImGui_ImplNullRender_RenderDrawData(ImGui::GetDrawData());
    };
    auto frames = [&](int n) {
        for (int i = 0; i < n; ++i) frame();
    };
    auto click = [&](float x, float y) {
        io.AddMousePosEvent(x, y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
    };
    auto key = [&](int chord) {
        const ImGuiKey k = ImGuiKey(chord & ~ImGuiMod_Mask_);
        if (chord & ImGuiMod_Ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(k, true);
        frame();
        io.AddKeyEvent(k, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
    };
    auto cursor = [&]() {
        ImGuiInputTextState* st = ImGui::GetInputTextState(logId);
        return st ? st->GetCursorPos() : -1;
    };
    frames(3);
    ImGuiWindow* child = nullptr;
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
        if (std::string(w->Name).find("Follow test/##log_") == 0) child = w;
    CHECK(child);
    if (!child) return;
    const ImGuiStyle& style = ImGui::GetStyle();
    auto atBottom = [&]() { return child->ScrollMax.y > 0 && child->Scroll.y == child->ScrollMax.y; };
    auto rowY = [&](size_t row) { return child->Pos.y + style.FramePadding.y - child->Scroll.y + float(row) * fh + fh * 0.5f; };

    // The caret starts at the end, and the box shows it.
    CHECK(view.following());
    CHECK_EQ(view.caret(), text.size());
    CHECK(atBottom());
    const float max0 = child->ScrollMax.y;
    add(30, 5);
    frames(2);
    CHECK(child->ScrollMax.y > max0);
    CHECK(atBottom());
    CHECK_EQ(view.caret(), text.size());
    // Scrolled back with the caret still at the end, new lines bring the box back to it.
    ImGui::SetScrollY(child, 0);
    frames(2);
    CHECK_EQ(child->Scroll.y, 0.0f);
    add(35, 2);
    frames(2);
    CHECK(atBottom());

    // Clicked on the empty last line: the caret is there, and new lines take it along.
    click(child->Pos.x + 30, rowY(size_t(std::count(text.begin(), text.end(), '\n'))));
    CHECK(ImGui::GetActiveID() == logId);
    CHECK_EQ(cursor(), int(text.size()));
    add(37, 3);
    frames(2);
    CHECK_EQ(cursor(), int(text.size()));
    CHECK(view.following());
    CHECK(atBottom());

    // Put on a line above, it stays there; scrolled back too, so does the box.
    click(child->Pos.x + 30, child->Pos.y + style.FramePadding.y + fh * 0.5f);
    const int mid = cursor();
    CHECK(mid > 0 && mid < int(text.size()) - 20);
    CHECK(!view.following());
    ImGui::SetScrollY(child, 0);
    frames(2);
    add(40, 3);
    frames(2);
    CHECK_EQ(cursor(), mid);
    CHECK_EQ(child->Scroll.y, 0.0f);
    // The log drops its oldest line: the caret stays on the same text.
    const std::string atCaret = text.substr(size_t(mid), 8);
    const size_t first = text.find('\n') + 1;
    text.erase(0, first);
    view.droppedFront(first);
    add(43, 1);
    frames(2);
    CHECK_EQ(cursor(), mid - int(first));
    CHECK_EQ(text.substr(size_t(cursor()), 8), atCaret);

    // Ctrl+End puts it back at the end, and it follows again.
    key(ImGuiMod_Ctrl | ImGuiKey_End);
    CHECK(view.following());
    add(44, 3);
    frames(2);
    CHECK_EQ(cursor(), int(text.size()));
    CHECK(atBottom());
    // Without the keyboard it follows the same way.
    click(20 + 450, 30 + 280);
    CHECK(ImGui::GetActiveID() != logId);
    add(47, 3);
    frames(2);
    CHECK_EQ(view.caret(), text.size());
    CHECK(atBottom());
    // Cleared, it starts again at the end.
    text.clear();
    frames(2);
    CHECK(view.following());
    CHECK_EQ(view.caret(), size_t(0));
    add(0, 2);
    frames(2);
    CHECK_EQ(view.caret(), text.size());
}

TEST("ui: File > Compression shows how gameplay.lzc is compressed; a change is an undo step, and saving writes it") {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "attribulina_compression_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    for (const char* f : {"ATTRIBUTES.BIN", "gameplay.lzc", "gameplay.bin"}) fs::copy_file(fs::path(ProStreetDir()) / f, tmp / f);
    const Bytes pack = ReadFile((tmp / "gameplay.bin").string());
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(tmp.string()));
    auto gameplay = [&]() -> LoadedPack* {
        for (const auto& p : a.database()->packs())
            if (p->compression) return p.get();
        return nullptr;
    };
    CHECK(gameplay());
    if (!gameplay()) return;
    CHECK(*gameplay()->compression == LzKind::Jdlz);
    CHECK(a.savedCompression(*gameplay()) == LzKind::Jdlz);
    CHECK(!a.hasUnsavedChanges());
    imgui.run(a, 2);
    auto choose = [&](const char* kind) {
        const test::ProbeItem* file = probe.find("File");
        CHECK(file);
        if (!file) return;
        ClickItem(imgui, a, *file);
        imgui.run(a, 1);
        const test::ProbeItem* menu = probe.find("Compression");
        CHECK(menu);
        if (!menu) return;
        ClickItem(imgui, a, *menu);
        imgui.run(a, 1);
        const test::ProbeItem* item = probe.find(kind);
        CHECK(item);
        if (item) ClickItem(imgui, a, *item);
        imgui.run(a, 2);
    };
    auto fileKind = [&]() {
        const auto h = ParseLzHeader(ReadFile((tmp / "gameplay.lzc").string()));
        return h ? std::optional<LzKind>(h->kind) : std::nullopt;
    };

    choose("HUFF");
    CHECK(*gameplay()->compression == LzKind::Huff);
    CHECK(a.hasUnsavedChanges());
    std::vector<std::string> labels = a.undoLabels();
    CHECK(!labels.empty() && labels.back() == "Compress gameplay.lzc with HUFF");
    CHECK(a.undo());
    CHECK(*gameplay()->compression == LzKind::Jdlz);
    CHECK(!a.hasUnsavedChanges());
    CHECK(a.redo());
    CHECK(*gameplay()->compression == LzKind::Huff);
    CHECK(a.hasUnsavedChanges());

    // Saved: the .lzc is HUFF, the .bin beside it the same pack, unpacked.
    CHECK(a.saveAll());
    CHECK(fileKind() == LzKind::Huff);
    CHECK(Decompress(ReadFile((tmp / "gameplay.lzc").string())) == pack);
    CHECK(ReadFile((tmp / "gameplay.bin").string()) == pack);
    CHECK(a.savedCompression(*gameplay()) == LzKind::Huff);
    CHECK(!a.hasUnsavedChanges());
    // Undone past the save, JDLZ is a change from the file again.
    CHECK(a.undo());
    CHECK(*gameplay()->compression == LzKind::Jdlz);
    CHECK(a.hasUnsavedChanges());
    CHECK(a.redo());
    CHECK(!a.hasUnsavedChanges());

    // RAWW stores it.
    choose("RAWW");
    CHECK(a.saveAll());
    CHECK(fileKind() == LzKind::Raww);
    CHECK_EQ(fs::file_size(tmp / "gameplay.lzc"), uintmax_t(pack.size() + 0x10));
    CHECK(Decompress(ReadFile((tmp / "gameplay.lzc").string())) == pack);
    // Choosing what it is already changes nothing.
    labels = a.undoLabels();
    choose("RAWW");
    CHECK(a.undoLabels() == labels);
    CHECK(!a.hasUnsavedChanges());
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: the title marks unsaved changes with * before the folder, however they were made") {
    platform::HeadlessImGui imgui;
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    imgui.run(a, 1);
    CHECK(host.title.find("Attribulina - ") == 0);
    CHECK(host.title.find("prostreet-vanilla (Pro Street on PC)") != std::string::npos);
    CHECK(a.runConsole("edit audio_revlimiter/default { total_frames_short = 9 }"));
    imgui.run(a, 1);
    CHECK(host.title.find("Attribulina - *") == 0);
    CHECK(host.title.find("prostreet-vanilla (Pro Street on PC)") != std::string::npos);
    CHECK(a.undo());
    imgui.run(a, 1);
    CHECK(host.title.find('*') == std::string::npos);
    // Core code that marks a pack (scripts, nfsms, gameplay commands) doesn't
    // call updateTitle: the next frame shows it all the same.
    a.database()->packs().front()->dirty = true;
    imgui.run(a, 1);
    CHECK(host.title.find("Attribulina - *") == 0);
}

TEST("ui: PathEventEnum values show as hex (0x...) and take hex with 0x or a decimal integer") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    Database* db = a.database();
    Collection* song = db->findCollection(StringHash32("music"), StringHash32("song19"));
    CHECK(song);
    if (!song) return;
    auto value = [&]() {
        const Attribute* pe = song->find(StringHash32("PathEvent"));
        return pe ? load32(pe->data.bytes.data(), Endian::Little) : 0u;
    };
    const Attribute* pe = song->find(StringHash32("PathEvent"));
    CHECK(pe);
    if (!pe) return;
    const ValueType vt = TypeOfAttribute(*pe, "PathEvent");
    CHECK_EQ(vt.hint, std::string("hex"));
    CHECK_EQ(app::FormatValue(*db, pe->data, 0, vt, Endian::Little), std::string("0x01F33C3A"));

    a.select(song, true);
    imgui.run(a, 2);
    const test::ProbeItem* filter = probe.find("##fieldfilter", "Collection");
    CHECK(filter);
    if (!filter) return;
    ClickItem(imgui, a, *filter);
    imgui.type(a, "PathEvent");
    imgui.run(a, 2);
    auto typeIn = [&](const char* text) {
        const test::ProbeItem* field = OnRow(probe, "PathEvent", "##int");
        CHECK(field);
        if (!field) return std::string();
        ClickItem(imgui, a, *field);
        const ImGuiInputTextState* st = ImGui::GetInputTextState(field->id);
        const std::string shown = st ? std::string(st->TextA.Data, size_t(st->TextLen)) : std::string();
        imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
        imgui.type(a, text);
        imgui.key(a, ImGuiKey_Enter);
        imgui.run(a, 2);
        return shown;
    };
    CHECK_EQ(typeIn("0x00ABCDEF"), std::string("0x01F33C3A"));
    CHECK_EQ(value(), 0x00ABCDEFu);
    CHECK_EQ(typeIn("305419896"), std::string("0x00ABCDEF")); // decimal
    CHECK_EQ(value(), 0x12345678u);
    CHECK_EQ(typeIn("-1"), std::string("0x12345678"));
    CHECK_EQ(value(), 0xFFFFFFFFu);
    CHECK_EQ(typeIn("0xdeadbeef"), std::string("0xFFFFFFFF"));
    CHECK_EQ(value(), 0xDEADBEEFu);
    // Hex digits without the 0x aren't a decimal integer: nothing changes.
    CHECK_EQ(typeIn("ABC"), std::string("0xDEADBEEF"));
    CHECK_EQ(value(), 0xDEADBEEFu);
    // Scripts write it the same way, and read it back.
    const std::string text = script::Print(script::NewScript(*db, script::Export(*db, {song})));
    CHECK(text.find("PathEvent = 0xDEADBEEF") != std::string::npos);
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: a new child, a duplicate, a copy and a paste take their name in CollectionName; a rename too, when it held the old one") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    Database* db = a.database();
    const KeyNames& names = db->names();
    const uint32_t field = StringHash32("CollectionName");
    auto text = [&](const Collection* c) {
        const Attribute* x = c ? c->resolve(field) : nullptr;
        const PtrSlot* s = x ? x->data.ptrAt(0) : nullptr;
        return s && s->isString ? s->text : std::string("(none)");
    };
    // A car whose CollectionName is its own name, as every one of them is.
    Collection* car = nullptr;
    for (Collection* c : db->findClass(StringHash32("pvehicle"))->collections)
        if (c->parent && c->children.empty() && c->find(field) && names.find(c->key) && text(c) == names.format(c->key)) {
            car = c;
            break;
        }
    CHECK(car);
    if (!car) return;
    const std::string name = names.format(car->key);

    Collection* dup = a.duplicate(car);
    CHECK(dup && dup != car);
    if (!dup) return;
    CHECK_EQ(names.format(dup->key), name + "_copy");
    CHECK_EQ(text(dup), name + "_copy");
    CHECK_EQ(text(car), name);
    Collection* root = a.copyHere(car, nullptr, false);
    CHECK(root && !root->parent);
    CHECK_EQ(text(root), names.format(root->key));
    CHECK(a.copyCollection(car, false, false));
    CHECK(a.paste(car, nullptr, false));
    Collection* pasted = a.selection();
    CHECK(pasted && pasted != car && pasted != dup && pasted != root);
    CHECK_EQ(text(pasted), names.format(pasted->key));

    // A new child, from the inspector's New Child button: its own name, not
    // the one it would inherit.
    a.select(car, true);
    imgui.run(a, 2);
    const test::ProbeItem* newChild = probe.find("New Child...", "Collection");
    CHECK(newChild);
    if (!newChild) return;
    ClickItem(imgui, a, *newChild);
    imgui.run(a, 2);
    imgui.type(a, "attribulina_child_test");
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    Collection* child = db->findCollection(car->classKey, StringHash32("attribulina_child_test"));
    CHECK(child && child->parent == car);
    CHECK(child && child->find(field));
    CHECK_EQ(text(child), std::string("attribulina_child_test"));
    CHECK_EQ(text(car), name);

    // A rename in the Explorer: CollectionName held the old name, so it follows.
    a.select(dup, true);
    imgui.run(a, 3);
    const test::ProbeItem* node = probe.find(name + "_copy", "##classes");
    CHECK(node);
    if (!node) return;
    ClickItem(imgui, a, *node);
    imgui.key(a, ImGuiKey_F2);
    imgui.run(a, 2);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
    imgui.type(a, "attribulina_collectionname_test");
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    CHECK_EQ(names.format(dup->key), std::string("attribulina_collectionname_test"));
    CHECK_EQ(text(dup), std::string("attribulina_collectionname_test"));
    // One that holds something else keeps it.
    Attribute& own = OverrideAttribute(*db, *pasted, *pasted->cls->field(field));
    EnsureStringSlot(own.data, 0).text = "something else";
    std::string oldName = names.format(pasted->key);
    CHECK(!SetCollectionNameField(*db, *pasted, &oldName));
    CHECK_EQ(text(pasted), std::string("something else"));
    CHECK(SetCollectionNameField(*db, *pasted));
    CHECK_EQ(text(pasted), oldName);
    // A class without the field is left alone.
    Collection* rev = db->findCollection(StringHash32("audio_revlimiter"), StringHash32("default"));
    CHECK(rev && !SetCollectionNameField(*db, *rev));
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: Save All writes every pack, changed or not, unless File > Save Only Changed Packs is on") {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "attribulina_saveall_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    for (const char* f : {"ATTRIBUTES.BIN", "FE_ATTRIB.BIN"}) fs::copy_file(fs::path(ProStreetDir()) / f, tmp / f);
    const Bytes attributes = ReadFile(ProStreetDir() + "/ATTRIBUTES.BIN"), fe = ReadFile(ProStreetDir() + "/FE_ATTRIB.BIN");
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(tmp.string()));
    CHECK_EQ(a.database()->packs().size(), size_t(2));
    imgui.run(a, 2);
    CHECK(!a.saveOnlyChanged());
    CHECK(!a.hasUnsavedChanges());
    auto fileMenu = [&](const char* label) {
        const test::ProbeItem* file = probe.find("File");
        CHECK(file);
        if (!file) return;
        ClickItem(imgui, a, *file);
        imgui.run(a, 1);
        const test::ProbeItem* item = probe.find(label);
        CHECK(item);
        if (item) ClickItem(imgui, a, *item);
        imgui.run(a, 2);
        if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            imgui.key(a, ImGuiKey_Escape); // a greyed-out item leaves the menu open
            imgui.run(a, 2);
        }
    };

    // Changed and deleted outside the editor, with nothing changed in it.
    std::ofstream(tmp / "ATTRIBUTES.BIN", std::ios::binary | std::ios::trunc) << "changed";
    fs::remove(tmp / "FE_ATTRIB.BIN");
    fileMenu("Save All");
    CHECK(ReadFile((tmp / "ATTRIBUTES.BIN").string()) == attributes);
    CHECK(fs::exists(tmp / "FE_ATTRIB.BIN") && ReadFile((tmp / "FE_ATTRIB.BIN").string()) == fe);
    CHECK(!a.hasUnsavedChanges());
    // Ctrl+S does the same.
    fs::remove(tmp / "FE_ATTRIB.BIN");
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_S);
    imgui.run(a, 1);
    CHECK(fs::exists(tmp / "FE_ATTRIB.BIN"));

    // Only changed packs: nothing changed is nothing to save.
    fileMenu("Save Only Changed Packs");
    CHECK(a.saveOnlyChanged());
    fs::remove(tmp / "FE_ATTRIB.BIN");
    fileMenu("Save All");
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_S);
    imgui.run(a, 1);
    CHECK(!fs::exists(tmp / "FE_ATTRIB.BIN"));
    // A change writes its pack only.
    CHECK(a.runConsole("edit audio_revlimiter/default { total_frames_short = 9 }"));
    imgui.run(a, 1);
    CHECK(a.hasUnsavedChanges());
    fileMenu("Save All");
    CHECK(!a.hasUnsavedChanges());
    CHECK(ReadFile((tmp / "ATTRIBUTES.BIN").string()) != attributes);
    CHECK(!fs::exists(tmp / "FE_ATTRIB.BIN"));
    fileMenu("Save Only Changed Packs");
    CHECK(!a.saveOnlyChanged());
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
    fs::remove_all(tmp);
}

TEST("ui: an early Most Wanted demo (64-bit keys): an edit to a reference undoes and redoes, and the names stay") {
    platform::HeadlessImGui imgui;
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(test::DataPath("attrib/databases/xbox/mw-early-oxm-betademo/ATTRIBUTES.BIN")));
    imgui.run(a, 1);
    CHECK(a.database() && a.database()->keys64());
    auto surface = [&]() {
        Database* db = a.database();
        Collection* car = db ? db->findCollection(StringHash32("pvehicle"), StringHash32("default")) : nullptr;
        const Attribute* x = car ? car->find(StringHash32("OnScrapeGround")) : nullptr;
        if (!x || x->elemSize != 56) return std::string("?");
        return app::FormatValue(*db, x->data, 0, TypeOfNamed(*db, "Attrib::RefSpec"), Endian::Little) + " " +
               std::to_string(load64(x->elem(0) + 8, Endian::Little) == StringHash64("attribulina_surface"));
    };
    const std::string before = surface();
    CHECK(before.rfind("simsurface/default", 0) == 0);
    CHECK(a.runConsole("edit pvehicle/default { OnScrapeGround[0].mSurface = simsurface/attribulina_surface }"));
    imgui.run(a, 1);
    CHECK_EQ(surface(), std::string("simsurface/attribulina_surface 1"));
    CHECK(a.undo());
    imgui.run(a, 1);
    CHECK_EQ(surface(), before);
    CHECK(a.redo());
    imgui.run(a, 1);
    CHECK_EQ(surface(), std::string("simsurface/attribulina_surface 1"));
    CHECK_EQ(Errors(a), size_t(0));
}

TEST("ui: the Lua functions listed are the open game's, the user's own first; Your Functions edits them") {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "attribulina_lua_lists_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "data" / "lua");
    fs::copy_file(test::DataPath("data/lua/prostreet_pc.txt"), tmp / "data" / "lua" / "prostreet_pc.txt");
    std::ofstream(tmp / "data" / "lua" / "carbon_pc.txt", std::ios::binary) << "# a test list\n"
                                                                               "table Game  # game functions\n"
                                                                               "function Game.OnlyInCarbon (n: number)  # one Carbon binds\n";
    // Undercover's two builds: the Wii's (Carbon's engine) and the current-gen one's.
    std::ofstream(tmp / "data" / "lua" / "undercover_wii.txt", std::ios::binary) << "table Game  # game functions\n"
                                                                                    "function Game.OnTheWii ()  # the Wii's build\n";
    std::ofstream(tmp / "data" / "lua" / "undercover_pc.txt", std::ios::binary) << "table Game  # game functions\n"
                                                                                   "function Game.OnThePC ()  # the current-gen build\n";
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    host.exeDir = tmp.string(); // its data/lua, and user_lua_<game>.txt beside it
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    auto find = [&](const std::string& name) -> const app::App::LuaEntry* {
        for (const auto& e : a.luaFunctions())
            if (e.name == name) return &e;
        return nullptr;
    };
    auto openAs = [&](const std::string& file) {
        a.closeAll();
        CHECK(a.open(file));
        imgui.run(a, 1);
    };

    openAs(ProStreetDir() + "/ATTRIBUTES.BIN");
    CHECK_EQ(a.luaFunctionList(), std::string("prostreet_pc"));
    CHECK(find("Debug.Print"));
    CHECK(!find("Game.OnlyInCarbon"));
    openAs(test::DataPath("attrib/databases/pc/carbon-vanilla/attributes.bin"));
    CHECK_EQ(a.luaFunctionList(), std::string("carbon_pc"));
    CHECK(find("Game.OnlyInCarbon"));
    CHECK(!find("Debug.Print"));
    // Carbon on the Wii has no list of its own: the PC's serves.
    openAs(test::DataPath("attrib/databases/wii/carbon-vanilla/attributes.bin"));
    CHECK_EQ(a.luaFunctionList(), std::string("carbon_pc"));
    // Undercover: the Wii's list serves its PS2 version (the same build), the PC's the 360's.
    openAs(test::DataPath("attrib/databases/wii/undercover-vanilla/attributes.bin"));
    CHECK_EQ(a.luaFunctionList(), std::string("undercover_wii"));
    CHECK(find("Game.OnTheWii") && !find("Game.OnThePC"));
    if (std::filesystem::exists(test::DataPath("attrib/databases/360/undercover-vanilla"))) {
        openAs(test::DataPath("attrib/databases/360/undercover-vanilla/ATTRIBUTES.BIN"));
        CHECK_EQ(a.luaFunctionList(), std::string("undercover_pc"));
    }
    a.closeAll();
    CHECK(a.openAs(test::DataPath("attrib/databases/wii/undercover-vanilla/attributes.bin"), *ParseTarget("undercover_ps2")));
    imgui.run(a, 1);
    CHECK_EQ(a.luaFunctionList(), std::string("undercover_wii"));
    // No list for Most Wanted here.
    openAs(test::DataPath("attrib/databases/pc/mw-vanilla/attributes.bin"));
    CHECK(a.luaFunctionList().empty());
    CHECK(a.luaFunctions().empty());

    // The user's own, for Pro Street.
    openAs(ProStreetDir() + "/ATTRIBUTES.BIN");
    a.openUserLuaFunctions();
    imgui.run(a, 2);
    CHECK(a.userLuaText().find("# Your own Lua functions for Pro Street") == 0);
    const int header = int(std::count(a.userLuaText().begin(), a.userLuaText().end(), '\n'));
    a.userLuaText() += "function MyHooks.SetNitro (amount: number)  # Fills the nitro.\n"
                       "method GEvent:MyCheck () -> boolean  # mine\n"
                       "function Debug.Print (text: string)  # Prints to my console (my hook).\n"
                       "function bad name\n"
                       "fuction Oops ()\n";
    imgui.run(a, 2);
    CHECK_EQ(a.userLuaErrors().size(), size_t(2));
    CHECK(a.userLuaErrors().count(header + 4) && a.userLuaErrors().count(header + 5));
    const test::ProbeItem* save = probe.find("Save", "Your Lua Functions");
    CHECK(save);
    if (save) ClickItem(imgui, a, *save);
    imgui.run(a, 1);
    CHECK(fs::exists(tmp / "user_lua_prostreet.txt"));
    CHECK(!a.luaFunctions().empty() && a.luaFunctions().front().name == "MyHooks.SetNitro" && a.luaFunctions().front().user);
    CHECK(find("GEvent:MyCheck"));
    CHECK(!find("Oops"));
    const app::App::LuaEntry* print = find("Debug.Print");
    CHECK(print && print->user && print->description.find("my hook") != std::string::npos);
    CHECK_EQ(std::count_if(a.luaFunctions().begin(), a.luaFunctions().end(), [](const auto& e) { return e.name == "Debug.Print"; }), 1);
    // They are Pro Street's: Carbon lists its own, and Pro Street's come back with it.
    openAs(test::DataPath("attrib/databases/pc/carbon-vanilla/attributes.bin"));
    CHECK(!find("MyHooks.SetNitro"));
    openAs(ProStreetDir() + "/ATTRIBUTES.BIN");
    CHECK(find("MyHooks.SetNitro"));
    // Lines the file can't give are named in the log as it is read.
    size_t warned = 0;
    for (const auto& l : a.log()) warned += l.text.find("user_lua_prostreet.txt:") == 0;
    CHECK(warned >= 2);
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: Lua list lines: what the user's file accepts") {
    std::string why;
    auto bad = [&](const char* line) {
        why.clear();
        return !app::App::ParseLuaLine(line, &why) && !why.empty();
    };
    auto ok = [&](const char* line) { return app::App::ParseLuaLine(line, &why).has_value(); };
    CHECK(ok("function Hooks.Set (n: number) -> boolean  # sets it"));
    CHECK(ok("function Hooks.Set(n: number)# sets it"));
    CHECK(ok("  method Car:Boost ()"));
    CHECK(ok("table Hooks"));
    CHECK(ok("function GlobalOne"));
    why.clear();
    CHECK(!app::App::ParseLuaLine("# a comment", &why) && why.empty());
    CHECK(!app::App::ParseLuaLine("   ", &why) && why.empty());
    CHECK(bad("method Car.Boost ()"));
    CHECK(bad("function Car:Boost ()"));
    CHECK(bad("function 9lives ()"));
    CHECK(bad("stdlib print"));
    CHECK(bad("function"));
    CHECK(bad("function Hooks.Set = 1"));
    const auto e = app::App::ParseLuaLine("function Hooks.Set (n: number) -> boolean  # sets it", &why);
    CHECK(e && e->kind == "function" && e->name == "Hooks.Set" && e->signature == "(n: number) -> boolean" && e->description == "sets it");
    // The program's lists are read as they are.
    const auto c = app::App::ParseLuaLine("constant vars.State.kState_NIS = 2  # GRacerInfo::State");
    CHECK(c && c->kind == "constant" && c->name == "vars.State.kState_NIS");
}

TEST("ui: data/lua: every list reads as the user's file must; Carbon and Undercover get their own") {
    namespace fs = std::filesystem;
    size_t files = 0;
    for (const auto& f : fs::directory_iterator(test::DataPath("data/lua"))) {
        if (f.path().extension() != ".txt") continue;
        ++files;
        std::ifstream in(f.path(), std::ios::binary);
        int n = 0;
        for (std::string line; std::getline(in, line);) {
            ++n;
            const auto le = app::App::ParseLuaLine(line);
            if (!le || le->kind == "constant" || le->kind == "stdlib") continue;
            std::string why;
            if (!app::App::ParseLuaLine(line, &why)) {
                std::printf("        %s:%d: %s\n", f.path().filename().string().c_str(), n, why.c_str());
                CHECK(false);
            }
        }
    }
    CHECK(files >= 3);

    platform::HeadlessImGui imgui;
    platform::HeadlessHost host; // the repo's data/lua
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    a.setUserLuaFolder((fs::temp_directory_path() / "attribulina_no_user_lua").string());
    auto find = [&](const std::string& name) -> const app::App::LuaEntry* {
        for (const auto& e : a.luaFunctions())
            if (e.name == name) return &e;
        return nullptr;
    };
    auto openAs = [&](const std::string& file) {
        a.closeAll();
        CHECK(a.open(file));
        imgui.run(a, 1);
    };
    openAs(test::DataPath("attrib/databases/pc/carbon-vanilla/attributes.bin"));
    CHECK_EQ(a.luaFunctionList(), std::string("carbon_pc"));
    CHECK(find("Camera.ActivateDefectorCamera"));
    CHECK(!find("Game.CalculateSectorPoints")); // Pro Street's
    CHECK(find("print") && find("print")->kind == "stdlib");
    openAs(test::DataPath("attrib/databases/wii/undercover-vanilla/attributes.bin"));
    CHECK_EQ(a.luaFunctionList(), std::string("undercover_wii"));
    CHECK(find("SPrintf"));
    openAs(ProStreetDir() + "/ATTRIBUTES.BIN");
    CHECK_EQ(a.luaFunctionList(), std::string("prostreet_pc"));
    CHECK(find("Game.CalculateSectorPoints"));
    CHECK(!find("Camera.ActivateDefectorCamera"));
}

TEST("ui: sequence builder: a Most Wanted handler compiles under the handler's own name; its functions are Most Wanted's") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    a.setUserLuaFolder((std::filesystem::temp_directory_path() / "attribulina_no_user_lua").string());
    CHECK(a.open(test::DataPath("attrib/databases/pc/mw-vanilla")));
    Database* db = a.database();
    CHECK(db && db->target().id() == "mw_pc");
    const std::string name = "stategraph_basicrace_done_handler_stateenter";
    Collection* h = db->findCollection(StringHash32("gameplay"), StringHash32(name));
    CHECK(h);
    if (!h) return;
    a.openStateGraphBuilder(h);
    imgui.run(a, 3);
    CHECK(a.sequenceHandler() == h);
    CHECK(a.sequenceStatus().find("same bytecode") != std::string::npos);
    std::string& src = a.sequenceSource();
    CHECK(src.find("function " + name + "(") != std::string::npos);
    CHECK_EQ(a.luaFunctionList(), std::string("mw_pc"));
    // An edit compiles under the same name; another name is refused.
    const size_t at = src.find("vars.RunningPostRace = false");
    CHECK(at != std::string::npos);
    src.insert(at, "Debug.Print(\"edited\")\n\t");
    CHECK(a.compileHandler());
    CHECK(lua::Decompile(HandlerChunk(*h)).source.find("\"edited\"") != std::string::npos);
    const std::string good = src;
    src = "function h_" + std::string("00000000( self, message, vars ) end\n");
    CHECK(!a.compileHandler());
    CHECK(a.sequenceStatus().find(name) != std::string::npos);
    src = good;
    // A plain copy of the handler takes its name in CollectionName, which the
    // game finds the function by, and its Lua defines that function.
    Collection* copy = a.duplicate(h);
    CHECK(copy && copy != h);
    if (copy) {
        CHECK_EQ(db->names().format(copy->key), name + "_copy");
        const Attribute* cn = copy->resolve(StringHash32("CollectionName"));
        const PtrSlot* text = cn ? cn->data.ptrAt(0) : nullptr;
        CHECK(text && text->text == name + "_copy");
        const std::string copied = lua::Decompile(HandlerChunk(*copy)).source;
        CHECK(copied.find("function " + name + "_copy(") != std::string::npos);
        CHECK(copied.find("\"edited\"") != std::string::npos);
        CHECK(lua::Decompile(HandlerChunk(*h)).source.find("function " + name + "(") != std::string::npos);
    }
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
    // The GameCube's files list the PC's functions.
    a.closeAll();
    CHECK(a.open(test::DataPath("attrib/databases/gamecube/mw-vanilla/ATTRIBUTES.BIN")));
    imgui.run(a, 1);
    CHECK_EQ(a.luaFunctionList(), std::string("mw_pc"));
    // The early Xbox demo: the same rule, its keys StringHash64.
    a.closeAll();
    CHECK(a.open(test::DataPath("attrib/databases/xbox/mw-early-oxm-betademo")));
    db = a.database();
    CHECK(db && db->keys64());
    if (!db) return;
    const std::string demoName = "stategraph_bin_race_engage_reset_handler_stateenter";
    Collection* dh = db->findCollection(StringHash32("gameplay"), StringHash32(demoName));
    CHECK(dh);
    if (!dh) return;
    a.openStateGraphBuilder(dh);
    imgui.run(a, 3);
    CHECK(a.sequenceHandler() == dh);
    CHECK(a.sequenceStatus().find("same bytecode") != std::string::npos);
    std::string& demoSrc = a.sequenceSource();
    CHECK(demoSrc.find("function " + demoName + "(") != std::string::npos);
    const size_t body = demoSrc.find('\n');
    CHECK(body != std::string::npos);
    demoSrc.insert(body + 1, "\tDebug.Print(\"demo\")\n");
    CHECK(a.compileHandler());
    CHECK(lua::Decompile(HandlerChunk(*dh)).source.find("\"demo\"") != std::string::npos);
    CHECK_EQ(a.luaFunctionList(), std::string("mw_pc"));
    CHECK_EQ(Errors(a), size_t(0));
}

TEST("ui: sequence builder: Undercover's 360 handlers edit and compile in Lua 5.1, its Wii version's in Lua 5.0") {
    for (const auto& [folder, version] : std::vector<std::pair<std::string, lua::Version>>{
             {"360/undercover-vanilla", lua::Version::Lua51}, {"wii/undercover-vanilla", lua::Version::Lua50}}) {
        if (!std::filesystem::exists(test::DataPath("attrib/databases/" + folder))) continue;
        platform::HeadlessImGui imgui;
        imgui.beforeFrame = ProbeBefore;
        imgui.afterFrame = ProbeAfter;
        test::UiProbe& probe = test::UiProbe::get();
        probe.conflicts.clear();
        platform::HeadlessHost host;
        app::App a(host);
        a.setup(1.0f, "");
        a.setUserKeysPath("");
        a.setUserLuaFolder((std::filesystem::temp_directory_path() / "attribulina_no_user_lua").string());
        CHECK(a.open(test::DataPath("attrib/databases/" + folder)));
        Database* db = a.database();
        CHECK(db);
        if (!db) continue;
        const std::string name = "basicrace/stategraph_basicrace_abandoned_handler_stateenter";
        Collection* h = db->findCollection(StringHash32("gameplay"), StringHash32(name));
        CHECK(h);
        if (!h) continue;
        CHECK(lua::DetectVersion(HandlerChunk(*h)) == version);
        a.openStateGraphBuilder(h);
        imgui.run(a, 3);
        CHECK(a.sequenceHandler() == h);
        CHECK(a.sequenceStatus().find("same bytecode") != std::string::npos);
        CHECK(!a.sequenceGraph().empty());
        CHECK_EQ(a.luaFunctionList(), std::string(version == lua::Version::Lua51 ? "undercover_pc" : "undercover_wii"));
        std::string& src = a.sequenceSource();
        const std::string fn = lua::HandlerFunctionName(name);
        CHECK(src.find("function " + fn + "(") != std::string::npos);
        const size_t body = src.find('\n', src.find("function " + fn + "("));
        CHECK(body != std::string::npos);
        src.insert(body + 1, "\tDebug.Print(\"edited\")\n");
        CHECK(a.compileHandler());
        CHECK(lua::DetectVersion(HandlerChunk(*h)) == version); // compiled with its own version
        CHECK(lua::Decompile(HandlerChunk(*h)).source.find("\"edited\"") != std::string::npos);
        // A copy renames its function and keeps the version.
        Collection* copy = a.duplicate(h);
        CHECK(copy && copy != h);
        if (copy) {
            CHECK(lua::DetectVersion(HandlerChunk(*copy)) == version);
            const std::string copied = lua::Decompile(HandlerChunk(*copy)).source;
            CHECK(copied.find("function " + lua::HandlerFunctionName(db->names().format(copy->key)) + "(") != std::string::npos);
            CHECK(copied.find("\"edited\"") != std::string::npos);
        }
        CHECK_EQ(Errors(a), size_t(0));
        for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
        CHECK(probe.conflicts.empty());
    }
}

TEST("ui: nfsms: Run All asks for the setup first; Run Line runs one line; Follow line opens the collection") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    Database* db = a.database();
    Collection* rev = db->findCollection(StringHash32("audio_revlimiter"), StringHash32("default"));
    auto u32 = [&](const char* f) { return load32(rev->find(StringHash32(f))->data.bytes.data(), Endian::Little); };
    a.setNfsmsText("version 4.6\n"                                                  // 1
                   "ui_image preview.png\n"                                         // 2
                   "ui_control checkbox loud false \"Longer cut\"\n"                 // 3
                   "update_field audio_revlimiter default total_frames_short 31\n"  // 4
                   "ui_option loud\n"                                                // 5
                   "update_field audio_revlimiter default total_frames_long 77\n"   // 6
                   "ui_option loud\n"                                                // 7
                   "update_field pvehicle street HornType 2\n");                     // 8
    imgui.run(a, 2);
    const test::ProbeItem* all = probe.find("Run All", "Script (nfsms)");
    CHECK(all);
    ClickItem(imgui, a, *all);
    imgui.run(a, 2);
    const test::ProbeItem* box = probe.find("Longer cut", "nfsms Setup");
    CHECK(box);
    ClickItem(imgui, a, *box);
    imgui.run(a, 1);
    ClickItem(imgui, a, probe.find("Run", "nfsms Setup"));
    imgui.run(a, 2);
    CHECK_EQ(u32("total_frames_short"), 31u);
    CHECK_EQ(u32("total_frames_long"), 77u);
    CHECK(db->packs().front()->dirty);

    // One line; the others stay.
    CHECK(a.runNfsms(4, 4) == true);
    CHECK(a.runNfsms(8, 8));
    Collection* street = db->findCollection(StringHash32("pvehicle"), StringHash32("street"));
    CHECK_EQ(int(street->find(StringHash32("HornType"))->data.bytes[0]), 2);

    // A failing line: off, the rest runs; strict, nothing changes.
    const std::string good = a.nfsmsText();
    a.setNfsmsText("update_field audio_revlimiter default total_frames_short 61\nupdate_field pvehicle nope HornType 1\n");
    CHECK(!a.runNfsms(0, 0));
    CHECK_EQ(u32("total_frames_short"), 61u);
    a.setNfsmsStrict(true);
    a.setNfsmsText("update_field audio_revlimiter default total_frames_short 62\nupdate_field pvehicle nope HornType 1\n");
    CHECK(!a.runNfsms(0, 0));
    CHECK_EQ(u32("total_frames_short"), 61u);
    a.setNfsmsStrict(false);
    a.setNfsmsText(good);
    imgui.run(a, 1);

    // Follow line: the caret on line 8 opens pvehicle/street.
    ClickItem(imgui, a, probe.find("Follow line", "Script (nfsms)"));
    a.select(rev, true);
    imgui.run(a, 2);
    const test::ProbeItem* editor = probe.find("##nfsms", "Script (nfsms)");
    CHECK(editor);
    imgui.click(a, editor->rect.Min.x + 60, editor->rect.Min.y + 8);
    imgui.run(a, 1);
    for (int i = 0; i < 7; ++i) imgui.key(a, ImGuiKey_DownArrow);
    imgui.run(a, 2);
    CHECK(a.selection() == street);
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: nfsms: the setup prompt shows each text above the controls after it") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    // As the real mods write it (no space after ui_text, a blank line between radio buttons).
    a.setNfsmsText("ui_text Grip difficulty\n"
                   "ui_control radiobutton dif option1 \"Easy\"\n"
                   "ui_text#\n"
                   "ui_control radiobutton dif option2 \"Hard\"\n"
                   "ui_text Drift difficulty\n"
                   "ui_control radiobutton diff option1 \"Normal drift\"\n"
                   "ui_text#\n"
                   "ui_text Extras\n"
                   "ui_control checkbox nos false \"More nitrous\"\n"
                   "ui_option dif option2\n"
                   "update_field audio_revlimiter default total_frames_short 33\n"
                   "ui_option dif option2\n");
    imgui.run(a, 2);
    ClickItem(imgui, a, probe.find("Run All", "Script (nfsms)"));
    imgui.run(a, 2);
    const test::ProbeItem* easy = probe.find("Easy", "nfsms Setup");
    const test::ProbeItem* hard = probe.find("Hard", "nfsms Setup");
    const test::ProbeItem* drift = probe.find("Normal drift", "nfsms Setup");
    const test::ProbeItem* nos = probe.find("More nitrous", "nfsms Setup");
    CHECK(easy && hard && drift && nos);
    if (!easy || !hard || !drift || !nos) return;
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float row = hard->rect.Min.y - easy->rect.Min.y; // the group's buttons sit together
    CHECK(row > 0 && row < line * 2);
    // "Drift difficulty" and the blank line written between Easy and Hard come
    // between the groups; "Extras" and a blank line between Normal drift and
    // the checkbox.
    CHECK(drift->rect.Min.y - hard->rect.Min.y > row + line * 1.5f);
    CHECK(nos->rect.Min.y - drift->rect.Min.y > row + line * 1.5f);
    // Nothing is chosen until the user picks.
    Collection* rev = a.database()->findCollection(StringHash32("audio_revlimiter"), StringHash32("default"));
    const uint32_t before = load32(rev->find(StringHash32("total_frames_short"))->data.bytes.data(), Endian::Little);
    ClickItem(imgui, a, *hard);
    imgui.run(a, 1);
    ClickItem(imgui, a, probe.find("Run", "nfsms Setup"));
    imgui.run(a, 2);
    CHECK(before != 33u);
    CHECK_EQ(load32(rev->find(StringHash32("total_frames_short"))->data.bytes.data(), Endian::Little), 33u);

    // To Mod Script: into the Script panel, asking first over unsaved text there.
    a.setScriptText("attribulina 1\n");
    imgui.run(a, 2);
    const test::ProbeItem* script = probe.find("##script", "Script");
    CHECK(script);
    if (!script) return;
    imgui.click(a, script->rect.Min.x + 40, script->rect.Min.y + 8);
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_End);
    imgui.type(a, "# work in progress");
    imgui.run(a, 2);
    a.setNfsmsText(std::string(a.nfsmsText())); // the nfsms window in front again (it shares the dock)
    imgui.run(a, 2);
    const test::ProbeItem* toAmod = probe.find("To Mod Script", "Script (nfsms)");
    CHECK(toAmod);
    if (!toAmod) return;
    ClickItem(imgui, a, *toAmod);
    imgui.run(a, 2);
    CHECK(a.scriptText().find("work in progress") != std::string::npos); // not yet
    const test::ProbeItem* replace = probe.find("Replace", "Replace the Script Panel's Script");
    CHECK(replace);
    if (replace) ClickItem(imgui, a, *replace);
    imgui.run(a, 2);
    CHECK(a.scriptText().rfind("attribulina 1", 0) == 0);
    CHECK(a.scriptText().find("option dif choice \"Grip difficulty\"") != std::string::npos);
    CHECK(a.scriptText().find("if $dif == \"Hard\"") != std::string::npos);
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: nfsms: an included script's failing line opens it in a tab; runs read the tab's text") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "attribulina_nfsms_tabs";
    fs::remove_all(dir);
    fs::create_directories(dir / "sub");
    {
        std::ofstream(dir / "main.nfsms") << "version 4.6\nscript\"sub\\inc.nfsms\"\nupdate_field audio_revlimiter default total_frames_short 44\n";
        std::ofstream(dir / "sub" / "inc.nfsms") << "update_field audio_revlimiter default total_frames_long 45\nupdate_field pvehicle nope HornType 1\n";
    }
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    Collection* rev = a.database()->findCollection(StringHash32("audio_revlimiter"), StringHash32("default"));
    auto u32 = [&](const char* f) { return load32(rev->find(StringHash32(f))->data.bytes.data(), Endian::Little); };
    std::ifstream in(dir / "main.nfsms");
    std::string mainText((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    a.setNfsmsText(mainText, (dir / "main.nfsms").string());
    imgui.run(a, 2);
    const std::string inc = attrib::nfsms::ScriptFile(dir.string(), "sub\\inc.nfsms");

    // Line 2 of sub\inc.nfsms fails: the script opens in a tab, in front.
    CHECK(!a.runNfsms(0, 0));
    imgui.run(a, 2);
    CHECK(a.nfsmsIncludedText(inc) != nullptr);
    CHECK(a.nfsmsActiveTab() == inc);
    CHECK(probe.find("inc.nfsms###" + inc, "Script (nfsms)"));
    CHECK_EQ(u32("total_frames_long"), 45u);

    // Fixed in the tab, not saved: the next run reads the tab.
    *a.nfsmsIncludedText(inc) = "update_field audio_revlimiter default total_frames_long 46\n";
    CHECK(a.runNfsms(0, 0));
    CHECK_EQ(u32("total_frames_long"), 46u);
    std::ifstream again{fs::path(inc)};
    CHECK(std::string((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>()).find("nope") != std::string::npos);

    // Closing it with unsaved changes asks; Discard closes it.
    imgui.run(a, 1);
    const test::ProbeItem* tab = probe.find("inc.nfsms###" + inc, "Script (nfsms)");
    CHECK(tab);
    if (!tab) return;
    // The tab's close button sits at its right end.
    imgui.click(a, tab->rect.Max.x - ImGui::GetFontSize() * 0.6f, (tab->rect.Min.y + tab->rect.Max.y) * 0.5f);
    imgui.run(a, 2);
    const test::ProbeItem* discard = probe.find("Discard", "Close Included Script");
    CHECK(discard);
    if (discard) ClickItem(imgui, a, *discard);
    imgui.run(a, 2);
    CHECK(a.nfsmsIncludedText(inc) == nullptr);

    // A message from an included line opens the script again.
    std::ofstream(fs::path(inc)) << "update_field pvehicle nope HornType 1\n";
    a.setNfsmsStrict(false);
    CHECK(!a.runNfsms(0, 0));
    imgui.run(a, 1);
    const test::ProbeItem* closeAgain = probe.find("inc.nfsms###" + inc, "Script (nfsms)");
    CHECK(closeAgain);
    if (closeAgain) imgui.click(a, closeAgain->rect.Max.x - ImGui::GetFontSize() * 0.6f, (closeAgain->rect.Min.y + closeAgain->rect.Max.y) * 0.5f);
    imgui.run(a, 2);
    CHECK(a.nfsmsIncludedText(inc) == nullptr);
    const test::ProbeItem* msg = nullptr;
    for (const auto& it : probe.items)
        if (it.window.find("##messages") != std::string::npos && it.label.find("inc.nfsms:1") != std::string::npos) msg = &it;
    CHECK(msg);
    if (msg) ClickItem(imgui, a, *msg);
    imgui.run(a, 2);
    CHECK(a.nfsmsIncludedText(inc) != nullptr);
    CHECK(a.nfsmsActiveTab() == inc);
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
    fs::remove_all(dir);
}

TEST("ui: changes made in the editor are recorded as nfsms and as a mod script") {
    namespace sc = attrib::script;
    platform::HeadlessImGui imgui;
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    Database* db = a.database();
    a.setRecording(true, true);
    imgui.run(a, 1); // the copy changes are measured from
    Collection* rev = db->findCollection(StringHash32("audio_revlimiter"), StringHash32("default"));
    Attribute* f = rev->find(StringHash32("total_frames_short"));
    const uint32_t old = load32(f->data.bytes.data(), Endian::Little);
    // A value edited as the inspector edits one, twice.
    store32(f->data.bytes.data(), 40, Endian::Little);
    a.markDirty(rev);
    imgui.run(a, 1);
    store32(f->data.bytes.data(), 41, Endian::Little);
    a.markDirty(rev);
    imgui.run(a, 1);
    CHECK_EQ(a.nfsmsText(), std::string("update_field audio_revlimiter default total_frames_short 41\n"));
    CHECK(a.scriptText().find("total_frames_short = 41 was " + std::to_string(old) + "\n") != std::string::npos);
    // A collection deleted from the Explorer.
    Collection* leaf = nullptr;
    for (Collection* c : db->findClass(StringHash32("pvehicle"))->collections)
        if (c->children.empty() && db->names().find(c->key)) leaf = c;
    const std::string leafName = db->names().format(leaf->key);
    CHECK(a.removeTree(leaf));
    imgui.run(a, 1);
    CHECK(a.nfsmsText().find("delete_node pvehicle " + leafName + "\n") != std::string::npos);
    CHECK(a.scriptText().find("delete pvehicle/" + leafName + "\n") != std::string::npos);
    // What a script changes isn't recorded.
    const std::string before = a.nfsmsText();
    CHECK(a.applyScript(sc::Parse("attribulina 1\nedit audio_revlimiter/default { total_frames_long = 5 }\n"), {}, false).ok);
    imgui.run(a, 2);
    CHECK_EQ(a.nfsmsText(), before);
    // Off: nothing more.
    a.setRecording(false, false);
    store32(f->data.bytes.data(), 42, Endian::Little);
    a.markDirty(rev);
    imgui.run(a, 1);
    CHECK_EQ(a.nfsmsText(), before);
}

TEST("ui: class designer: a new class is made from its design; the game's classes show read only, copy, delete and restore") {
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    imgui.run(a, 2);
    Database* db = a.database();
    auto click = [&](const char* label, const char* window) {
        const test::ProbeItem* it = probe.find(label, window);
        if (!it) std::printf("        no %s in %s\n", label, window);
        ClickItem(imgui, a, it);
    };

    // A design, then Create Class.
    a.openClassDesigner(nullptr);
    ClassDesign& d = a.classDesigner().draft;
    d.name = "mod_racer";
    d.key = StringHash32(d.name);
    d.fields.clear();
    for (const auto& [name, type, layout, array] : std::vector<std::tuple<const char*, const char*, bool, bool>>{
             {"SPEED", "EA::Reflection::Float", true, false},
             {"TARGET", "Attrib::RefSpec", true, false},
             {"COUNT", "EA::Reflection::Int32", false, false},
             {"NAMES", "Attrib::StringKey", false, true}}) {
        FieldDesign f;
        f.name = name;
        f.key = StringHash32(name);
        f.typeHash = StringHash32(type);
        f.storage = layout ? FieldStorage::Layout : FieldStorage::Entry;
        f.array = array;
        d.fields.push_back(f);
    }
    imgui.run(a, 2);
    CHECK_EQ(probe.count("Create Class", "Class Designer"), 1);
    click("Create Class", "Class Designer");
    imgui.run(a, 2);
    ClassDef* mine = db->findClass(StringHash32("mod_racer"));
    CHECK(mine != nullptr);
    CHECK_EQ(mine->fields.size(), size_t(4));
    CHECK(a.hasUnsavedChanges());
    CHECK(a.undoLabels().back() == "New class mod_racer");
    CHECK_EQ(probe.count("Apply Changes", "Class Designer"), 1); // now it edits the class it made

    // Its menu in the tree (a collection of it, revealed, scrolls the tree
    // there; the designer, floating over the tree, is closed first).
    a.classDesigner().open = false;
    CHECK(a.runConsole("create mod_racer/racer_one under root in vault db"));
    a.select(db->findCollection(mine->key, StringHash32("racer_one")), true);
    imgui.run(a, 3);
    const test::ProbeItem* node = nullptr;
    for (const auto& it : probe.items)
        if (it.label.rfind("mod_racer  (", 0) == 0) node = &it;
    Rightclick(imgui, a, node);
    CHECK_EQ(probe.count("Class Design...", "##Popup"), 1);
    CHECK_EQ(probe.count("Attrib::Gen Header...", "##Popup"), 1);
    CHECK_EQ(probe.count("Delete Class...", "##Popup"), 1);
    imgui.key(a, ImGuiKey_Escape);
    imgui.run(a, 1);

    // The header tab shows its Attrib::Gen class.
    a.openClassDesigner(mine, true);
    imgui.run(a, 2);
    const std::string header = a.designerHeader();
    CHECK(header.find("class mod_racer : private Instance") != std::string::npos);
    CHECK(probe.find("Save As...", "Class Designer") != nullptr);
    const std::string saved = (std::filesystem::temp_directory_path() / "attribulina_mod_racer.h").string();
    host.nextSaveFile = saved;
    click("Save As...", "Class Designer");
    CHECK(std::filesystem::exists(saved) && ReadFile(saved).size() == header.size());
    std::filesystem::remove(saved);

    // A class of the game: read only, and Copy as New Class makes an editable design of it.
    ClassDef* pv = db->findClass(StringHash32("pvehicle"));
    a.openClassDesigner(pv);
    imgui.run(a, 2);
    CHECK(a.classDesigner().readOnly);
    CHECK_EQ(probe.count("Create Class", "Class Designer"), 0);
    CHECK_EQ(probe.count("Apply Changes", "Class Designer"), 0);
    click("Copy as New Class", "Class Designer");
    imgui.run(a, 1);
    CHECK(!a.classDesigner().readOnly);
    CHECK_EQ(a.classDesigner().draft.name, std::string("pvehicle_copy"));
    CHECK_EQ(probe.count("Create Class", "Class Designer"), 1);
    CHECK(!a.applyClassDesign() || db->findClass(StringHash32("pvehicle_copy")) != nullptr);

    // Delete Class asks first, then the class and its collections go.
    a.classDesigner().open = false;
    a.select(db->findCollection(mine->key, StringHash32("racer_one")), true);
    imgui.run(a, 3);
    node = nullptr;
    for (const auto& it : probe.items)
        if (it.label.rfind("mod_racer  (", 0) == 0) node = &it;
    Rightclick(imgui, a, node);
    click("Delete Class...", "##Popup");
    imgui.run(a, 2);
    CHECK(probe.find("Delete", "Delete Class") != nullptr);
    click("Delete", "Delete Class");
    imgui.run(a, 2);
    CHECK(db->findClass(StringHash32("mod_racer")) == nullptr);
    CHECK(a.selection() == nullptr);
    CHECK(a.tabs().empty());

    // Undo brings it back, and the game's classes stay locked in the database undo made.
    CHECK(a.undo());
    imgui.run(a, 2);
    db = a.database();
    CHECK(db->findClass(StringHash32("mod_racer")) != nullptr);
    CHECK(IsGameClass(*db, StringHash32("pvehicle")));

    // A game class, deleted, is offered back and restored as the retail files define it.
    ClassDef* sm = db->findClass(StringHash32("smackable"));
    const ClassDesign before = DesignOf(*db, *sm);
    CHECK(a.removeClass(sm));
    imgui.run(a, 1);
    CHECK_EQ(MissingRetailClasses(*db).size(), size_t(1));
    CHECK(a.restoreGameClass(StringHash32("smackable")));
    imgui.run(a, 2);
    CHECK(db->findClass(StringHash32("smackable")) != nullptr);
    CHECK(DesignOf(*db, *db->findClass(StringHash32("smackable"))) == before);
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& x : probe.conflicts) std::printf("        ID conflict: %s\n", x.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: classes made, changed, deleted and restored in the editor are recorded as class statements") {
    namespace sc = attrib::script;
    platform::HeadlessImGui imgui;
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir() + "/ATTRIBUTES.BIN"));
    Database* db = a.database();
    a.setRecording(true, true);
    imgui.run(a, 1);

    a.openClassDesigner(nullptr);
    ClassDesign& d = a.classDesigner().draft;
    d.name = "rec_class";
    d.key = StringHash32(d.name);
    d.fields[0].name = "SPEED";
    d.fields[0].key = StringHash32("SPEED");
    CHECK(a.applyClassDesign());
    imgui.run(a, 1);
    CHECK(a.scriptText().find("class rec_class in vault db") != std::string::npos);
    CHECK(a.scriptText().find("    layout SPEED EA::Reflection::Float\n") != std::string::npos);
    CHECK(a.nfsmsText().find("# class rec_class: nfsms has no command") != std::string::npos);

    FieldDesign extra;
    extra.name = "EXTRA";
    extra.key = StringHash32(extra.name);
    extra.typeHash = StringHash32("EA::Reflection::Int32");
    a.classDesigner().draft.fields.push_back(extra);
    CHECK(a.applyClassDesign());
    imgui.run(a, 1);
    CHECK(a.scriptText().find("    entry EXTRA EA::Reflection::Int32\n") != std::string::npos);

    CHECK(a.removeClass(db->findClass(StringHash32("smackable"))));
    imgui.run(a, 1);
    CHECK(a.scriptText().find("delete class smackable\n") != std::string::npos);
    CHECK(a.restoreGameClass(StringHash32("smackable")));
    imgui.run(a, 1);
    CHECK(a.scriptText().find("class smackable in vault db") != std::string::npos);

    // The recorded script makes the same classes from the files.
    Database fresh;
    for (const auto& s : db->classLists()) fresh.addClassList(s);
    fresh.addTypeSchema(db->sharedTypeSchema());
    fresh.names() = db->names();
    fresh.setTarget(db->target());
    fresh.loadPack(ProStreetDir() + "/ATTRIBUTES.BIN");
    const sc::ApplyReport r = sc::Apply(fresh, sc::Parse(a.scriptText(), "recorded.amod"));
    for (const auto& m : r.messages)
        if (m.level == sc::Message::Level::Error) std::printf("        %s\n", m.text.c_str());
    CHECK(r.ok);
    for (const char* name : {"rec_class", "smackable"}) {
        const ClassDef* x = fresh.findClass(StringHash32(name));
        const ClassDef* y = db->findClass(StringHash32(name));
        CHECK(x && y);
        if (x && y) CHECK(DesignOf(fresh, *x) == DesignOf(*db, *y));
    }
    CHECK_EQ(Errors(a), size_t(0));
}

namespace {
// The Explorer's Files tree: a pack's or vault's node, by the start of its label.
const test::ProbeItem* FilesNode(const std::string& start) {
    for (const auto& it : test::UiProbe::get().items)
        if (it.label.rfind(start, 0) == 0 && it.window.find("##files") != std::string::npos) return &it;
    return nullptr;
}
} // namespace

TEST("ui: the Files tree makes, renames and deletes vaults, each an undo step") {
    if (!std::filesystem::exists(ProStreetDir() + "/gameplay.lzc")) return;
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(ProStreetDir()));
    imgui.run(a, 2);
    ClickItem(imgui, a, probe.find("Files"));
    imgui.run(a, 1);
    auto menu = [&](const test::ProbeItem* node, const char* item) {
        Rightclick(imgui, a, node);
        ClickItem(imgui, a, probe.find(item, "##Popup"));
        imgui.run(a, 2);
    };

    // A new vault in gameplay.lzc, from the pack's menu.
    menu(FilesNode("gameplay.lzc"), "New Vault...");
    imgui.type(a, "aaa_ui_vault");
    imgui.run(a, 1);
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    CHECK(FindVault(*a.database(), "aaa_ui_vault"));
    CHECK(a.hasUnsavedChanges());
    // Undone and redone.
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_Z);
    imgui.run(a, 2);
    CHECK(!FindVault(*a.database(), "aaa_ui_vault"));
    CHECK(!a.hasUnsavedChanges());
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_Y);
    imgui.run(a, 2);
    CHECK(FindVault(*a.database(), "aaa_ui_vault"));

    // Renamed from its own menu (it sorts first in the open pack).
    if (const test::ProbeItem* pack = FilesNode("gameplay.lzc")) ClickItem(imgui, a, *pack);
    imgui.run(a, 2);
    menu(FilesNode("aaa_ui_vault  ("), "Rename Vault...");
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
    imgui.type(a, "aab_ui_vault");
    imgui.run(a, 1);
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    CHECK(!FindVault(*a.database(), "aaa_ui_vault") && FindVault(*a.database(), "aab_ui_vault"));
    // A taken name is refused.
    menu(FilesNode("aab_ui_vault  ("), "Rename Vault...");
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_A);
    imgui.type(a, "GPCORE");
    imgui.run(a, 1);
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    imgui.key(a, ImGuiKey_Escape);
    imgui.run(a, 2);
    CHECK(FindVault(*a.database(), "aab_ui_vault"));

    // Deleted; the database's vault can be neither.
    menu(FilesNode("aab_ui_vault  ("), "Delete Vault...");
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    CHECK(!FindVault(*a.database(), "aab_ui_vault"));
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_Z);
    imgui.run(a, 2);
    CHECK(FindVault(*a.database(), "aab_ui_vault"));
    if (const test::ProbeItem* attrs = FilesNode("ATTRIBUTES.BIN  (")) ClickItem(imgui, a, *attrs);
    imgui.run(a, 2);
    menu(FilesNode("db  ("), "Delete Vault...");
    CHECK(FindVault(*a.database(), "db"));
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
}

TEST("ui: World: a new event vault file is written by Save All; deleted from its vault's menu, saving removes the file") {
    namespace fs = std::filesystem;
    const fs::path from = test::DataPath("attrib/databases/pc/world-final");
    if (!fs::exists(from)) return;
    const fs::path tmp = fs::temp_directory_path() / "attribulina_event_vault_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "gc.vaults");
    fs::copy_file(from / "attributes.bin", tmp / "attributes.bin");
    fs::copy_file(from / "gc.vaults" / "1_2_1_sprint.bin", tmp / "gc.vaults" / "1_2_1_sprint.bin");
    platform::HeadlessImGui imgui;
    imgui.beforeFrame = ProbeBefore;
    imgui.afterFrame = ProbeAfter;
    test::UiProbe& probe = test::UiProbe::get();
    probe.conflicts.clear();
    platform::HeadlessHost host;
    app::App a(host);
    a.setup(1.0f, "");
    a.setUserKeysPath("");
    CHECK(a.open(tmp.string()));
    CHECK(a.database() && a.database()->packs().size() == 2);
    if (!a.database()) return;
    imgui.run(a, 2);
    ClickItem(imgui, a, probe.find("Files"));
    imgui.run(a, 1);

    // From the tree's own menu, below its rows.
    const test::ProbeItem* last = FilesNode("1_2_1_sprint.bin  (");
    CHECK(last);
    if (!last) return;
    imgui.click(a, last->rect.Min.x + 20, last->rect.Max.y + 60, 1);
    imgui.run(a, 1);
    ClickItem(imgui, a, probe.find("New Event Vault File...", "##Popup"));
    imgui.run(a, 2);
    imgui.type(a, "attribulina_ui_event");
    imgui.run(a, 1);
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    const fs::path file = tmp / "gc.vaults" / "attribulina_ui_event.bin";
    auto pack = [&]() -> const LoadedPack* {
        for (const auto& p : a.database()->packs())
            if (fs::path(p->path) == file) return p.get();
        return nullptr;
    };
    CHECK(pack() && pack()->format == PackFormat::VaultFile && pack()->dirty && !fs::exists(file));
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_Z);
    imgui.run(a, 2);
    CHECK(!pack() && !a.hasUnsavedChanges());
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_Y);
    imgui.run(a, 2);
    CHECK(pack() && a.hasUnsavedChanges());
    CHECK(a.saveAll());
    CHECK(!a.hasUnsavedChanges() && fs::exists(file));
    {
        const PackFile pf = ReadPack(ReadFile(file.string()));
        CHECK(pf.format == PackFormat::VaultFile && pf.vaults.size() == 1 && pf.vaults[0].name == "attribulina_ui_event");
    }

    // Deleted from its vault's menu: saving moves the file aside.
    imgui.run(a, 1);
    ClickItem(imgui, a, FilesNode("attribulina_ui_event.bin  ("));
    imgui.run(a, 1);
    Rightclick(imgui, a, FilesNode("attribulina_ui_event  ("));
    ClickItem(imgui, a, probe.find("Delete Vault...", "##Popup"));
    imgui.run(a, 2);
    imgui.key(a, ImGuiKey_Enter);
    imgui.run(a, 2);
    CHECK(!pack() && a.hasUnsavedChanges() && fs::exists(file));
    // Undone, it is the file as saved; redone, it goes again.
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_Z);
    imgui.run(a, 2);
    CHECK(pack() && !pack()->dirty && !a.hasUnsavedChanges());
    imgui.key(a, ImGuiMod_Ctrl | ImGuiKey_Y);
    imgui.run(a, 2);
    CHECK(!pack() && a.hasUnsavedChanges());
    CHECK(a.saveAll());
    CHECK(!a.hasUnsavedChanges() && !fs::exists(file) && fs::exists(fs::path(file.string() + ".bak")));
    CHECK_EQ(Errors(a), size_t(0));
    for (const auto& c : probe.conflicts) std::printf("        ID conflict: %s\n", c.c_str());
    CHECK(probe.conflicts.empty());
    fs::remove_all(tmp);
}
