// nfsms (core/Nfsms.h): reading NFS-VltEd / Attribulator scripts, running each
// command on Pro Street, the setup blocks, and the lines the editor records.

#include "Test.h"

#include "core/Curve.h"
#include "core/Edit.h"
#include "core/Folder.h"
#include "core/Game.h"
#include "core/Hash.h"
#include "core/Nfsms.h"
#include "core/Script.h"
#include "core/Values.h"
#include "core/Writer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace attrib;
namespace sc = attrib::script;
namespace ns = attrib::nfsms;

namespace {

std::string Vanilla(const char* file) { return test::DataPath(std::string("attrib/databases/pc/prostreet-vanilla/") + file); }

std::unique_ptr<Database> Load(bool gameplay = false) {
    auto db = std::make_unique<Database>();
    db->addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
    db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
    for (const char* f : {"ATTRIBUTES.BIN", "cars_vault.BIN", "FE_ATTRIB.BIN"}) db->loadPack(Vanilla(f));
    if (gameplay) db->loadPack(Vanilla("gameplay.lzc"));
    return db;
}

std::string Messages(const sc::ApplyReport& r) {
    std::string s;
    for (const sc::Message& m : r.messages) s += std::to_string(m.pos.line) + ": " + m.text + "\n";
    return s;
}

sc::ApplyReport Run(Database& db, const std::string& text, ns::RunSettings st = {}) {
    const ns::Script s = ns::Parse(text, "test.nfsms");
    if (st.choices.checkboxes.empty() && st.choices.radios.empty()) st.choices = ns::DefaultChoices(s);
    return ns::Run(db, s, st);
}

#define CHECK_RUN(r)                                                                 \
    do {                                                                             \
        if (!(r).ok) std::fprintf(stderr, "%s", Messages(r).c_str());                \
        CHECK((r).ok);                                                               \
    } while (0)

Collection& Col(Database& db, const char* cls, const char* name) {
    Collection* c = db.findCollection(StringHash32(cls), StringHash32(name));
    if (!c) std::fprintf(stderr, "no %s/%s\n", cls, name);
    CHECK(c != nullptr);
    return *c;
}
Attribute& Own(Collection& c, const char* field) {
    Attribute* a = c.find(StringHash32(field));
    if (!a) std::fprintf(stderr, "no own %s\n", field);
    CHECK(a != nullptr);
    return *a;
}
float F32(const Attribute& a, uint32_t i = 0, uint32_t off = 0) { return loadf32(a.data.bytes.data() + i * a.elemSize + off, Endian::Little); }
uint32_t U32(const Attribute& a, uint32_t i = 0, uint32_t off = 0) { return load32(a.data.bytes.data() + i * a.elemSize + off, Endian::Little); }

size_t Changes(const sc::Script& s) {
    size_t n = 0;
    for (const sc::Statement& st : s.statements)
        n += st.kind != sc::Statement::Kind::Header && st.kind != sc::Statement::Kind::Meta && st.kind != sc::Statement::Kind::Comment;
    return n;
}

} // namespace

namespace {
// A game folder as the editor opens it: every schema, its game and platform
// from its files.
std::unique_ptr<Database> LoadFolder(const std::string& dir) {
    namespace fs = std::filesystem;
    auto db = std::make_unique<Database>();
    db->setTarget(DetectTarget(dir).target);
    for (const auto& e : fs::directory_iterator(test::DataPath("data/schema")))
        if (e.path().extension() == ".txt") db->addTypeSchema(TypeSchema::Load(e.path().string()));
    db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
    for (const FolderFile& f : FolderPacks(dir))
        if (f.skipped.empty()) db->loadPack(f.path);
    return db;
}

// The steps after the field and its index, as text: .mKitNumber, .X[0].
std::string StepsText(const sc::FieldPath& p, size_t from) {
    std::string s;
    for (size_t i = from; i < p.steps.size(); ++i)
        s += p.steps[i].member.empty() ? "[" + std::to_string(p.steps[i].index) + "]" : "." + p.steps[i].member;
    return s;
}
} // namespace

TEST("nfsms: every platform of a game runs it; Nitro, which NFS-VltEd never read, refuses") {
    for (const char* platform : {"pc", "360", "ps2", "wii"}) {
        Database db;
        db.loadPack(test::DataPath(std::string("attrib/databases/") + platform + "/prostreet-vanilla/ATTRIBUTES.BIN"));
        CHECK_EQ(ns::GameCode(db), std::string("PS"));
        CHECK(ns::Incompatibility(db).empty());
        CHECK_RUN(Run(db, "update_field audio_revlimiter default total_frames_short 9"));
        Collection& c = Col(db, "audio_revlimiter", "default");
        const Attribute& a = Own(c, "total_frames_short");
        CHECK_EQ(ReadInteger(a.data, 0, TypeOfAttribute(a, "total_frames_short"), c.vault->raw.endian), int64_t(9));
    }
    // A word past FEPartData's bools: byte 24 on the PC, 36 on the PS2, where
    // they are 4 bytes. NFS-VltEd's word 7 is LogoTextureId on both.
    for (const char* platform : {"pc", "ps2"}) {
        Database db;
        db.addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/prostreet_") + platform + ".txt")));
        db.loadPack(test::DataPath(std::string("attrib/databases/") + platform + "/prostreet-vanilla/ATTRIBUTES.BIN"));
        db.loadPack(test::DataPath(std::string("attrib/databases/") + platform + "/prostreet-vanilla/FE_ATTRIB.BIN"));
        Collection* part = nullptr;
        std::string field;
        uint32_t key = 0;
        for (const auto& cls : db.classes())
            for (Collection* c : cls->collections)
                for (const Attribute& a : c->attributes)
                    if (!part && a.type && a.type->name == "FEPartData" && !a.isArray) {
                        part = c;
                        key = a.key;
                        field = db.names().format(a.key);
                    }
        CHECK(part);
        if (!part) continue;
        ns::RunSettings st;
        st.names = ns::PropertyNames::Load(test::DataPath("data/nfsms/prostreet_pc.txt"));
        CHECK_RUN(Run(db, "update_field " + db.names().format(part->classKey) + " " + db.names().format(part->key) + " " + field +
                              " 7 0x12345678\n", st));
        const Attribute* a = part->find(key);
        CHECK(a);
        if (a) CHECK_EQ(load32(a->data.bytes.data() + (std::string(platform) == "ps2" ? 36 : 24), part->vault->raw.endian), 0x12345678u);
    }
    // An early Most Wanted demo: NFS-VltEd's PC words reach its 24-byte
    // references, which store StringHash64 keys.
    for (const char* demo : {"xbox/mw-early-oxm-betademo", "ps2/mw-early-opm-betademo"}) {
        auto db = LoadFolder(test::DataPath(std::string("attrib/databases/") + demo));
        CHECK_EQ(ns::GameCode(*db), std::string("MW"));
        ns::RunSettings st;
        st.names = ns::PropertyNames::Load(test::DataPath("data/nfsms/mw_pc.txt"));
        CHECK_RUN(Run(*db, "update_field pvehicle default OnScrapeGround[0] SurfaceCollectionKey organic\n"
                           "update_field pvehicle default OnScrapeGround[0] EffectCollectionKey carscrapeearth\n"
                           "update_field pvehicle default OnScrapeGround[0] MaxSpeed 31\n", st));
        Collection& car = Col(*db, "pvehicle", "default");
        const Attribute* a = car.find(StringHash32("OnScrapeGround"));
        CHECK(a && a->elemSize == 56);
        if (!a) continue;
        CHECK_EQ(load64(a->elem(0), Endian::Little), StringHash64("simsurface"));
        CHECK_EQ(load64(a->elem(0) + 8, Endian::Little), StringHash64("organic"));
        CHECK_EQ(load64(a->elem(0) + 32, Endian::Little), StringHash64("carscrapeearth"));
        CHECK(loadf32(a->elem(0) + 52, Endian::Little) == 31.0f);
        // Recorded back in the same words.
        const sc::Script edits = sc::Parse("attribulina 1\nedit pvehicle/default { OnScrapeGround[0].mSurface = simsurface/concrete }\n", "rec.amod");
        const std::vector<std::string> lines = ns::FromStatements(*db, edits.statements, st.names.get());
        CHECK(std::find(lines.begin(), lines.end(), "update_field pvehicle default OnScrapeGround[0] SurfaceCollectionKey concrete") != lines.end());
    }
    Database nitro;
    nitro.setTarget({Game::Nitro, Platform::Wii});
    CHECK(ns::GameCode(nitro).empty());
    const std::string why = ns::Incompatibility(nitro);
    CHECK(why.find("NFS-VltEd's, which never read") != std::string::npos);
    const sc::ApplyReport r = Run(nitro, "update_field audio_revlimiter default total_frames_short 9");
    CHECK(!r.ok);
    CHECK_EQ(r.messages.size(), size_t(1));
    CHECK_EQ(r.messages[0].text, why);
    sc::ApplyReport conv;
    const sc::Script out = ns::ToModScript(nitro, ns::Parse("update_field audio_revlimiter default total_frames_short 9", "x.nfsms"), {}, &conv);
    CHECK(out.statements.empty());
    CHECK_EQ(conv.errors(), size_t(1));
}


TEST("nfsms: each game's words lead to the same member on every platform of it, and record back as themselves") {
    struct GameTable {
        const char* table;
        std::vector<const char*> folders;
        std::vector<std::string> expectedFailures; // "type word" that can't lead anywhere on some platform
        // "type word" over several of our members: a change to one records by our member names.
        std::vector<std::string> recordedOtherwise = {};
    };
    const std::vector<GameTable> games = {
        // A curve's allocatedMemory is the game's to set; scripts can't.
        {"prostreet_pc", {"pc/prostreet-vanilla", "360/prostreet-vanilla", "ps2/prostreet-vanilla", "wii/prostreet-vanilla"},
         {"Curve AllocatedMemory"}},
        // Unk lands on padding (data/nfsms/mw_pc.txt). The early demos' LeaderSupport and
        // TrafficPatternRecord don't have retail's last members yet.
        {"mw_pc",
         {"pc/mw-vanilla", "xbox/mw-vanilla", "gamecube/mw-vanilla", "360/mw-beta", "ps2/mw-beta", "xbox/mw-early-oxm-betademo",
          "ps2/mw-early-opm-betademo"},
         {"ControllerDataRecord Unk", "LeaderSupport PriorityChance", "LeaderSupport PriorityTime", "TrafficPatternRecord 6"}},
        // carbon_ps2.txt has no layout for the PS2's 20-byte SteeringSensitivityParameter (Data sets it).
        {"carbon_pc",
         {"pc/carbon-vanilla", "ps3/carbon-vanilla", "wii/carbon-vanilla", "ps2/carbon-vanilla", "gamecube/carbon-vanilla", "360/carbon-latedemo"},
         {"SteeringSensitivityParameter CurveMultiplierHighSpeed", "SteeringSensitivityParameter CurveMultiplierLowSpeed",
          "SteeringSensitivityParameter CurvePower", "SteeringSensitivityParameter CurveStyle", "SteeringSensitivityParameter InnerDeadZone",
          "SteeringSensitivityParameter NumberOfSteps", "SteeringSensitivityParameter OuterDeadZone"}},
        // NFS-VltEd's Undercover words are for its PC version; the Wii's types are Carbon's, and four differ.
        // A class reference's Collection is its pointer, and a curve's allocatedMemory the game's (in
        // the 48-byte Curve and what holds one). NFS-VltEd reads Sound::JittererParams as eight u16s
        // (its words set half of a float) and VinylLayer as seven u32s over several members.
        {"undercover_pc", {"pc/undercover-vanilla", "360/undercover-vanilla", "wii/undercover-vanilla"},
         {"Attrib::Gen::ClassRefSpec_presetride Collection", "Curve AllocatedMemory", "AI::GlueCurve Easy_AllocatedMemory",
          "AI::GlueCurve Hard_AllocatedMemory", "AxlePairCurve Front_AllocatedMemory", "AxlePairCurve Rear_AllocatedMemory",
          "CameraCurveReactionRecord 13", "Sound::JittererParams 2", "Sound::JittererParams 4",
          "Sound::JittererParams 6", "Sound::JittererParams 8", "CollisionReactionRecord Elasticity", "EffectLinkageRecord AudioClassKey", "EffectLinkageRecord AudioCollectionKey",
          "EffectLinkageRecord SFXMaxSpeed", "EffectLinkageRecord SFXMinSpeed", "GMapRegionInfo BoundsMaxX", "GMapRegionInfo BoundsMaxY",
          "GMapRegionInfo BoundsMinX", "GMapRegionInfo BoundsMinY", "TireEffectRecord EmitterLowLodClass", "TireEffectRecord EmitterLowLodCollection",
          "TireEffectRecord TireCondition"},
         {"VinylsAttrib::VinylLayer 3", "VinylsAttrib::VinylLayer 4"}},
        // A curve's allocatedMemory is the game's to set, and a StringKey's hash
        // comes from its text (NFS-VltEd's word 1 of IntegratedKitWheelOffset).
        {"world_pc", {"pc/world-final"}, {"Curve AllocatedMemory", "IntegratedKitWheelOffset 1"}},
        // NFS-VltEd's WB: World's betas, which the final build's schema lays out. It
        // reads CameraCauseAndEffect as 27 numbers, its two curves' array pointers,
        // lengths and allocatedMemory among them, which nothing can set.
        {"world_beta_pc",
         {"pc/world-2010-beta", "pc/world-2009-beta"},
         {"Curve AllocatedMemory", "CameraCauseAndEffect 8", "CameraCauseAndEffect 9", "CameraCauseAndEffect 10",
          "CameraCauseAndEffect 11", "CameraCauseAndEffect 12", "CameraCauseAndEffect 13", "CameraCauseAndEffect 14",
          "CameraCauseAndEffect 21", "CameraCauseAndEffect 22", "CameraCauseAndEffect 23", "CameraCauseAndEffect 24",
          "CameraCauseAndEffect 25", "CameraCauseAndEffect 26", "CameraCauseAndEffect 27"}},
    };
    size_t wrong = 0; // checked at the end, so every game and platform reports
    for (const GameTable& g : games) {
        auto names = ns::PropertyNames::Load(test::DataPath(std::string("data/nfsms/") + g.table + ".txt"));
        ns::RunSettings st;
        st.names = names;
        std::map<std::string, std::string> first; // "type word" -> where it led on the first platform
        std::set<std::string> failures;
        for (const char* folder : g.folders) {
            auto db = LoadFolder(test::DataPath(std::string("attrib/databases/") + folder));
            CHECK(ns::NamesFor(*db, {names}) == names);
            // A value of each type: the first field of it that has an element.
            std::map<std::string, std::pair<Collection*, std::string>> values;
            for (const auto& cls : db->classes())
                for (Collection* c : cls->collections)
                    for (const Attribute& a : c->attributes) {
                        if (!a.type || !a.count || values.count(a.type->name)) continue;
                        std::string field = db->names().format(a.key);
                        if (a.isArray) field += "[0]";
                        values[a.type->name] = {c, field};
                    }
            size_t tried = 0;
            for (const auto& [typeName, type] : names->types) {
                if (type.single) continue;
                auto it = values.find(typeName.substr(0, typeName.find("(MODEL)")));
                if (it == values.end() || typeName.find("(MODEL)") != std::string::npos) continue;
                auto [c, field] = it->second;
                for (const ns::PropertyNames::Property& pr : type.properties) {
                    // A reference's pointer, which the game sets: NFS-VltEd names it, nothing can set it.
                    const std::string path = pr.pathText();
                    if (path.size() >= 14 && path.compare(path.size() - 14, 14, "mCollectionPtr") == 0) continue;
                    const std::string key = typeName + " " + pr.word;
                    std::vector<std::string> words{pr.word};
                    if (pr.list) words = {pr.word, "0", pr.elements.empty() ? std::string("Value") : pr.elements[0]};
                    std::string part;
                    sc::FieldPath p;
                    try {
                        p = ns::WordsLeadTo(*db, *c, field, words, st, &part);
                    } catch (const std::exception& e) {
                        failures.insert(key);
                        if (std::find(g.expectedFailures.begin(), g.expectedFailures.end(), key) == g.expectedFailures.end())
                            std::printf("        %s on %s: %s\n", key.c_str(), folder, e.what());
                        continue;
                    }
                    ++tried;
                    const size_t from = field.find('[') != std::string::npos ? 1 : 0;
                    const std::string where = StepsText(p, from) + (part.empty() ? "" : " " + part);
                    if (!first.count(key)) first[key] = where;
                    else if (first[key] != where) {
                        std::printf("        %s on %s: %s, not %s\n", key.c_str(), folder, where.c_str(), first[key].c_str());
                        ++wrong;
                    }
                    // Recorded, the same value comes out in the same word.
                    if (part == "data" || pr.list) continue;
                    sc::Statement set;
                    set.kind = sc::Statement::Kind::Set;
                    set.path = p;
                    if (part.empty()) {
                        set.value = sc::MakeNumber("1");
                    } else {
                        auto v = std::make_shared<sc::Value>();
                        v->kind = sc::Value::Kind::Binary;
                        v->op = '/';
                        v->items = {sc::MakeString("a"), sc::MakeString("b")};
                        set.value = v;
                    }
                    sc::Statement edit;
                    edit.kind = sc::Statement::Kind::Edit;
                    edit.target.cls = db->names().format(c->classKey);
                    edit.target.name = db->names().format(c->key);
                    edit.body.push_back(set);
                    bool found = false;
                    for (const std::string& line : ns::FromStatements(*db, {edit}, names.get())) {
                        const ns::Command cmd = ns::ParseLine(line);
                        found |= cmd.args.size() > 4 && std::find(cmd.args.begin() + 3, cmd.args.end() - 1, pr.word) != cmd.args.end() - 1;
                    }
                    if (!found && std::find(g.recordedOtherwise.begin(), g.recordedOtherwise.end(), key) != g.recordedOtherwise.end()) continue;
                    if (!found) {
                        std::printf("        %s on %s records as:", key.c_str(), folder);
                        for (const std::string& line : ns::FromStatements(*db, {edit}, names.get())) std::printf(" [%s]", line.c_str());
                        std::printf("\n");
                        ++wrong;
                    }
                }
            }
            if (!tried) ++wrong;
        }
        if (failures != std::set<std::string>(g.expectedFailures.begin(), g.expectedFailures.end())) {
            for (const std::string& f : failures) std::printf("        %s fails: %s\n", g.table, f.c_str());
            ++wrong;
        }
    }
    CHECK_EQ(wrong, size_t(0));
}

TEST("nfsms: lines split the way NFS-VltEd splits them") {
    ns::Command c = ns::ParseLine("  update_field pvehicle \"my car\" ENGINE[0] Torque \"3 0\"  # set it", 4);
    CHECK_EQ(c.name, std::string("update_field"));
    CHECK(c.args == (std::vector<std::string>{"pvehicle", "my car", "ENGINE[0]", "Torque", "3 0"}));
    CHECK_EQ(c.comment, std::string(" set it"));
    CHECK_EQ(c.line, 4);
    // ui_text runs to a #, as NFS-VltEd's pattern reads it.
    c = ns::ParseLine("ui_text Adds a turbo: # and this is a comment\t\r");
    CHECK(c.args == std::vector<std::string>{"Adds a turbo:"});
    CHECK(ns::ParseLine("# just a comment").name.empty());
    CHECK(ns::ParseLine("// also one").name.empty());
    CHECK(ns::ParseLine("   ").name.empty());
    c = ns::ParseLine("update_field a b c \"\"");
    CHECK(c.args.back().empty());

    // From a real mod: no space after the command, \ in paths, ui_text# as a
    // blank line, and notes that aren't commands.
    c = ns::ParseLine("ui_text#\r");
    CHECK_EQ(c.name, std::string("ui_text"));
    CHECK(c.args == std::vector<std::string>{""});
    c = ns::ParseLine("ui_text------------\r");
    CHECK(c.args == std::vector<std::string>{"------------"});
    c = ns::ParseLine("script\"Main\\Changes in behaviour\\AI Curves.nfsms\"\r");
    CHECK_EQ(c.name, std::string("script"));
    CHECK(c.args == std::vector<std::string>{"Main\\Changes in behaviour\\AI Curves.nfsms"});
    c = ns::ParseLine("delete_nodes pvehicle");
    CHECK_EQ(c.name, std::string("delete_nodes")); // the longest command, not delete_node + "s"
    for (const char* note : {"-", "Balance!", "Suspension Rework", "upate_field a b c 1"}) {
        c = ns::ParseLine(note);
        CHECK(c.name.empty());
        CHECK_EQ(c.comment, std::string(note));
    }
    CHECK_EQ(ns::CommandAt("  ui_text#"), size_t(7));
    CHECK_EQ(ns::CommandAt("Balance!"), size_t(0));
    CHECK_EQ(ns::ScriptFile("mod", "Main\\a.nfsms"), (std::filesystem::path("mod") / "Main" / "a.nfsms").string());

    const ns::Script notes = ns::Parse("version 4.6\nfrobnicate a b\n-\n", "bad.nfsms");
    CHECK_EQ(notes.commands.size(), size_t(1));
    CHECK(!notes.hasSetup());
    const ns::Script s = ns::Parse("ui_text A mod\nui_image pic.png\nui_control checkbox kits false \"Body kits\"\n"
                                   "ui_control radiobutton tier street \"Street\"\nui_control radiobutton tier race \"Race\"\n");
    CHECK_EQ(s.texts.size(), size_t(1));
    CHECK_EQ(s.images.size(), size_t(1));
    CHECK_EQ(s.controls.size(), size_t(3));
    CHECK(s.hasSetup());
    CHECK(s.hasDescription());
    const ns::Choices d = ns::DefaultChoices(s);
    CHECK(!d.checkboxes.at("kits"));
    CHECK(d.radios.empty()); // no radio button starts chosen, as in NFS-VltEd
    // A bare ui_option is an error line in NFS-VltEd and doesn't end a block.
    CHECK(!ns::Parse("ui_option\n").commands[0].error.empty());
}

TEST("nfsms: the setup prompt lists texts and controls in NFS-VltEd's order") {
    // NFS-VltEd puts a radio button group in a panel where its first button
    // is, so the texts between the buttons show after the group.
    const ns::Script s = ns::Parse("ui_text Pick one\n"
                                   "ui_control radiobutton dif option1 \"Easy\"\n"
                                   "ui_text#\n"
                                   "ui_control radiobutton dif option2 \"Hard\"\n"
                                   "ui_text Then\n"
                                   "ui_control radiobutton diff option1 \"Normal\"\n"
                                   "ui_control checkbox extra false \"Extra\"\n"
                                   "ui_control radiobutton diff option2 \"Hard\"\n");
    using Kind = ns::SetupItem::Kind;
    CHECK_EQ(s.setup.size(), size_t(6));
    if (s.setup.size() != 6) return;
    CHECK(s.setup[0].kind == Kind::Text && s.setup[0].text == "Pick one");
    CHECK(s.setup[1].kind == Kind::Radios && s.setup[1].controls == (std::vector<int>{0, 1}));
    CHECK(s.setup[2].kind == Kind::Text && s.setup[2].text.empty());
    CHECK(s.setup[3].kind == Kind::Text && s.setup[3].text == "Then");
    CHECK(s.setup[4].kind == Kind::Radios && s.setup[4].controls == (std::vector<int>{2, 4}));
    CHECK(s.setup[5].kind == Kind::Checkbox && s.setup[5].controls == (std::vector<int>{3}));
}

TEST("nfsms: a ui_option block ends at the next ui_option line, whatever it names") {
    auto db = Load();
    Database& d = *db;
    // As the mods write it: the opening line repeated to close the block.
    const std::string script = "ui_control radiobutton dif option1 \"Easy\"\n"
                               "ui_control radiobutton dif option2 \"Hard\"\n"
                               "ui_option dif option1\n"
                               "update_field audio_revlimiter default load_for_short 0.25\n"
                               "ui_option dif option1\n"
                               "ui_option dif option2\n"
                               "update_field audio_revlimiter default load_for_short 0.75\n"
                               "ui_option dif option2\n"
                               "update_field audio_revlimiter default total_frames_short 9\n";
    Collection& rev = Col(d, "audio_revlimiter", "default");
    const float before = F32(Own(rev, "load_for_short"));
    CHECK_RUN(Run(d, script)); // nothing chosen: neither block runs
    CHECK(F32(Own(rev, "load_for_short")) == before);
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 9u);
    ns::RunSettings st;
    st.choices.radios["dif"] = "option2";
    CHECK_RUN(Run(d, script, st));
    CHECK(F32(Own(rev, "load_for_short")) == 0.75f);
}

TEST("nfsms: NFS-VltEd's own words for the parts of a value (data/nfsms), as a real mod writes them") {
    auto db = Load();
    Database& d = *db;
    ns::RunSettings st;
    st.names = ns::PropertyNames::Load(test::DataPath("data/nfsms/prostreet_pc.txt"));
    CHECK(ns::NamesFor(d, {st.names}) == st.names); // the table for PS
    std::string data;
    for (int i = 0; i < 28; ++i) data += (i ? " " : "") + std::string(i == 3 ? "3F" : i == 2 ? "80" : "00");
    const std::string script = "update_field vehicle 240sx TORQUE MinY 90\n"                                  // Curve: yMin
                               "update_collection vehicle 240sx TORQUE YValues 2 Value 0.5\n"                // a point of Y
                               "update_field presetride player_d_day Parts[7] KitNumber 3\n"                 // mKitNumber, not the u32 before it
                               "update_field presetride player_d_day Parts[7] PartClassKey car_part\n"       // into the RefSpec mPart
                               "update_field part_physics tires_lvl_4_kit03 UpgradeDescriptions[0] Type tires_2\n"
                               "update_field part_physics tires_lvl_4_kit03 UpgradeDescriptions[0] 4 0.75\n" // mBlendingPower
                               "update_field controller drive LowSensitivitySteeringParameters Data \"" + data + "\"\n";
    sc::ApplyReport r = Run(d, script, st);
    CHECK_RUN(r);
    CHECK_EQ(r.errors(), size_t(0));
    const CurveValue torque = ReadCurve(Own(Col(d, "vehicle", "240sx"), "TORQUE"), 0, Endian::Little);
    CHECK(torque.yMin == 90.0f);
    CHECK(torque.y.size() > 2 && torque.y[2] == 0.5f);
    const Attribute& parts = Own(Col(d, "presetride", "player_d_day"), "Parts");
    CHECK_EQ(U32(parts, 7, 20), 3u);
    CHECK_EQ(U32(parts, 7, 4), StringHash32("car_part"));
    const Attribute& up = Own(Col(d, "part_physics", "tires_lvl_4_kit03"), "UpgradeDescriptions");
    CHECK_EQ(U32(up, 0, 4), StringHash32("tires_2"));
    CHECK(F32(up, 0, 12) == 0.75f);
    CHECK(F32(Own(Col(d, "controller", "drive"), "LowSensitivitySteeringParameters"), 0, 0) == 1.0f); // 00 00 80 3F
    // A word the type doesn't have names the ones it does.
    r = Run(d, "update_field vehicle 240sx TORQUE GraphScale 1\n", st);
    CHECK_EQ(r.errors(), size_t(1));
    CHECK(Messages(r).find("MinX") != std::string::npos);
    // Our member names work too, with or without the m.
    CHECK_RUN(Run(d, "update_field presetride player_d_day Parts[7] mKitNumber 4\nupdate_field vehicle 240sx TORQUE yMax 300\n", st));
    CHECK_EQ(U32(Own(Col(d, "presetride", "player_d_day"), "Parts"), 7, 20), 4u);

    // Recording writes NFS-VltEd's words back, and a curve point as update_collection.
    const sc::Script edits = sc::Parse("attribulina 1\n"
                                       "edit presetride/player_d_day { Parts[7].mKitNumber = 5 }\n"
                                       "edit vehicle/240sx { TORQUE.yMin = 100\n TORQUE.Y[2] = 0.25 }\n"
                                       "edit part_physics/tires_lvl_4_kit03 { UpgradeDescriptions[0].mBlendingPower = 2 }\n",
                                       "rec.amod");
    const std::vector<std::string> lines = ns::FromStatements(d, edits.statements, st.names.get());
    auto has = [&](const std::string& l) {
        const bool ok = std::find(lines.begin(), lines.end(), l) != lines.end();
        if (!ok) std::fprintf(stderr, "missing: %s\n", l.c_str());
        return ok;
    };
    CHECK(has("update_field presetride player_d_day Parts[7] KitNumber 5"));
    CHECK(has("update_field vehicle 240sx TORQUE MinY 100"));
    CHECK(has("update_collection vehicle 240sx TORQUE YValues 2 Value 0.25"));
    CHECK(has("update_field part_physics tires_lvl_4_kit03 UpgradeDescriptions[0] 4 2"));
    // And they read back.
    std::string text;
    for (const std::string& l : lines) text += l + "\n";
    CHECK_RUN(Run(d, text, st));
    CHECK_EQ(U32(Own(Col(d, "presetride", "player_d_day"), "Parts"), 7, 20), 5u);
}

TEST("nfsms: a real NFS-VltEd mod (nfsms test/, when present) runs with no failed line") {
    namespace fs = std::filesystem;
    const std::string mod = test::DataPath("nfsms test/Vlted/Click Me To Install!.nfsms");
    if (!fs::exists(fs::path(mod))) {
        std::printf("        (skipped: no %s)\n", mod.c_str());
        return;
    }
    auto db = Load(true);
    std::ifstream in(fs::path(mod), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const ns::Script s = ns::Parse(ss.str(), "Click Me To Install!.nfsms", fs::path(mod).parent_path().string());
    // The setup as the mod's author wrote it: texts between groups of choices.
    CHECK(s.hasDescription());
    CHECK_EQ(s.controls.size(), size_t(26)); // 4 + 3 + 2 radio buttons, 17 boxes
    ns::RunSettings st;
    st.names = ns::PropertyNames::Load(test::DataPath("data/nfsms/prostreet_pc.txt"));
    st.choices = ns::DefaultChoices(s);
    st.choices.radios = {{"dif", "option2"}, {"diff", "option1"}, {"point", "option1"}};
    for (auto& [box, on] : st.choices.checkboxes) on = true;
    const sc::ApplyReport r = ns::Run(*db, s, st);
    if (r.errors()) std::fprintf(stderr, "%s", Messages(r).substr(0, 2000).c_str());
    CHECK(r.ok);
    CHECK_EQ(r.errors(), size_t(0));
    CHECK(r.valuesSet > 10000);

    // As a mod script with the same choices, it makes the same database.
    auto fresh = Load(true);
    auto scratch = sc::CloneDatabase(*fresh);
    sc::ApplyReport conv;
    const sc::Script amod = sc::Parse(sc::Print(ns::ToModScript(*scratch, s, st, &conv)), "mod.amod");
    CHECK_EQ(conv.errors(), size_t(0));
    sc::ApplySettings as;
    as.options = {{"dif", "Normal"}, {"diff", "Normal"}, {"point", "Tweaked Standing Points"}};
    for (const ns::Control& c : s.controls)
        if (c.kind == ns::Control::Kind::Checkbox) as.options[c.name] = "true";
    const sc::ApplyReport ar = sc::Apply(*fresh, amod, as);
    if (!ar.ok) std::fprintf(stderr, "%s", Messages(ar).substr(0, 2000).c_str());
    CHECK(ar.ok);
    const sc::Script diff = sc::Diff(*db, *fresh);
    if (Changes(diff)) std::fprintf(stderr, "%s", sc::Print(diff).substr(0, 3000).c_str());
    CHECK_EQ(Changes(diff), size_t(0));
}

TEST("nfsms: ToModScript writes a mod script that does what the nfsms script does") {
    auto db = Load();
    const std::string included = "update_field audio_revlimiter default total_frames_long 21\n";
    const std::string script =
        "version 4.6\n"
        "game PS\n"
        "ui_text A test mod.\n"
        "ui_text Pick the length\n"
        "ui_control radiobutton len short \"Short\"\n"
        "ui_control radiobutton len long \"Long\"\n"
        "ui_control checkbox loud true \"Louder\"\n"
        "update_field audio_revlimiter default total_frames_short 11\n"
        "update_field vehicle 240sx TORQUE MinY 95\n"                                  // NFS-VltEd's words
        "update_collection vehicle 240sx TORQUE YValues 1 Value 0.25\n"
        "update_field presetride player_d_day Parts[7] PartClassKey car_part\n"
        "ui_option len long\n"
        "script\"sub\\more.nfsms\"\n"
        "ui_option len long\n"
        "ui_option loud\n"
        "update_field audio_revlimiter default cutoff_frames_short 3\n"
        "ui_option loud\n"
        "copy_node pvehicle 240sx 240sx_conv\n"                                         // a root copy
        "add_node pvehicle 240sx 240sx_child\n"
        "rename_node pvehicle 240sx_child 240sx_kid\n"
        "move_node pvehicle 240sx_kid 240sx_conv\n"
        "add_field pvehicle 240sx_kid HornType\n"
        "update_field pvehicle 240sx_kid HornType 4\n"
        "resize_field skidaudio default ShockSqueakThreshPavement 3\n"
        "file_copy readme.txt docs/readme.txt\n"
        "delete_node pvehicle 240sx_conv\n";
    ns::RunSettings st;
    st.names = ns::PropertyNames::Load(test::DataPath("data/nfsms/prostreet_pc.txt"));
    st.readFile = [&](const std::string& p) {
        CHECK(p.find("more.nfsms") != std::string::npos);
        return included;
    };
    const ns::Script s = ns::Parse(script, "mod.nfsms", "/mods/x");
    sc::ApplyReport conv;
    auto scratch = sc::CloneDatabase(*db);
    const sc::Script out = ns::ToModScript(*scratch, s, st, &conv);
    const std::string text = sc::Print(out);
    if (conv.errors()) std::fprintf(stderr, "%s\n%s", Messages(conv).c_str(), text.c_str());
    CHECK_EQ(conv.errors(), size_t(1)); // file_copy: no readme.txt in /mods/x; it stays a comment
    CHECK(text.find("option len choice \"Pick the length\" choices [\"(none)\", \"Short\", \"Long\"] default \"(none)\"") != std::string::npos);
    CHECK(text.find("option loud bool \"Louder\" default true") != std::string::npos);
    CHECK(text.find("if $len == \"Long\"") != std::string::npos);
    CHECK(text.find("description \"A test mod.\\nPick the length\"") != std::string::npos);
    CHECK(text.find("file_copy readme.txt") != std::string::npos);
    // It reads back, and with the same choices it does what the nfsms script does.
    const sc::Script back = sc::Parse(text, "mod.amod");
    for (const auto& [len, loud] : std::vector<std::pair<std::string, bool>>{{"long", false}, {"short", true}}) {
        auto viaAmod = sc::CloneDatabase(*db);
        sc::ApplySettings as;
        as.options = {{"len", len == "long" ? "Long" : "Short"}, {"loud", loud ? "true" : "false"}};
        const sc::ApplyReport ar = sc::Apply(*viaAmod, back, as);
        if (!ar.ok) std::fprintf(stderr, "%s", Messages(ar).c_str());
        CHECK(ar.ok);
        auto viaNfsms = sc::CloneDatabase(*db);
        ns::RunSettings rs = st;
        rs.choices = ns::DefaultChoices(s);
        rs.choices.radios["len"] = len;
        rs.choices.checkboxes["loud"] = loud;
        ns::Run(*viaNfsms, s, rs);
        const sc::Script diff = sc::Diff(*viaNfsms, *viaAmod);
        if (Changes(diff)) std::fprintf(stderr, "%s", sc::Print(diff).c_str());
        CHECK_EQ(Changes(diff), size_t(0));
    }
}

TEST("nfsms: update_field reaches each kind of value") {
    auto db = Load();
    Database& d = *db;
    const ValueType axle = TypeOfAttribute(Own(Col(d, "vehicle", "default"), "SHOCK_EXT_STIFFNESS"), "SHOCK_EXT_STIFFNESS");
    std::string member = axle.st->members[1].name;
    for (char& ch : member) ch = char(std::tolower(static_cast<unsigned char>(ch))); // names match without case
    const uint32_t axleOff = axle.st->members[1].offset;
    const ValueType hud = TypeOfAttribute(Own(Col(d, "controller", "drive"), "ControllerHudType"), "ControllerHudType");
    const std::string script = "version 4.6\n"
                               "game PS\n"
                               "update_field audio_revlimiter default load_for_short 0.5\n"
                               "update_field audio_revlimiter default total_frames_short 0x10\n"
                               "update_field skidaudio default ShockSqueakThreshPavement[1] 5\n"
                               "update_field skidaudio default ShockSqueakThreshPavement[^] 6\n"
                               "update_field vehicle default SHOCK_EXT_STIFFNESS[0] " + member + " 7.25\n"
                               "update_field effects default EmitterQuadratic Y 3\n"
                               "update_field fe_global default default_car_tuner Collection street\n"
                               "update_field pvehicle default BEHAVIOR_MECHANIC_RIGIDBODY \"my body\"\n"
                               "update_field car mclarenf1 isBoosterPack True\n"
                               "update_field controller drive ControllerHudType " + hud.en->values[1].first + "\n"
                               "update_collection collisionsfx_damage hood VolMap[0] some_name\n";
    const sc::ApplyReport r = Run(d, script);
    CHECK_RUN(r);
    CHECK_EQ(r.warnings(), size_t(0));
    Collection& rev = Col(d, "audio_revlimiter", "default");
    CHECK(F32(Own(rev, "load_for_short")) == 0.5f);
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 16u);
    const Attribute& sk = Own(Col(d, "skidaudio", "default"), "ShockSqueakThreshPavement");
    CHECK(F32(sk, 1) == 5 && F32(sk, sk.count - 1) == 6);
    CHECK(F32(Own(Col(d, "vehicle", "default"), "SHOCK_EXT_STIFFNESS"), 0, axleOff) == 7.25f);
    CHECK(F32(Own(Col(d, "effects", "default"), "EmitterQuadratic"), 0, 4) == 3);
    const Attribute& ref = Own(Col(d, "fe_global", "default"), "default_car_tuner");
    CHECK_EQ(U32(ref, 0, 4), StringHash32("street")); // the collection; the class stays
    CHECK(U32(ref, 0, 0) != 0);
    const Attribute& key = Own(Col(d, "pvehicle", "default"), "BEHAVIOR_MECHANIC_RIGIDBODY");
    CHECK(key.data.ptrAt(4) && key.data.ptrAt(4)->text == "my body");
    CHECK_EQ(int(Own(Col(d, "car", "mclarenf1"), "isBoosterPack").data.bytes[0]), 1);
    CHECK_EQ(int64_t(U32(Own(Col(d, "controller", "drive"), "ControllerHudType"))), hud.en->values[1].second);
    CHECK_EQ(U32(Own(Col(d, "collisionsfx_damage", "hood"), "VolMap")), StringHash32("some_name")); // a name in a number field

    // A line that fails is reported at its line; the script goes on.
    auto fails = [&](const std::string& line, const char* what) {
        const sc::ApplyReport x = Run(d, "version 4.6\n" + line + "\n");
        if (x.errors() != 1 || Messages(x).find(what) == std::string::npos) std::fprintf(stderr, "%s", Messages(x).c_str());
        CHECK(x.ok);
        CHECK_EQ(x.errors(), size_t(1));
        CHECK(Messages(x).find(what) != std::string::npos);
    };
    fails("update_field pvehicle nope HornType 1", "2: class 'pvehicle' does not contain node 'nope'");
    fails("update_field pvehicle street NOPE 1", "does not contain 'NOPE'");
    fails("update_field pvehicle street HornType 300", "out of range");
    fails("update_field effects default EmitterQuadratic 3", "takes more than one value");
    fails("update_field car mclarenf1 isBoosterPack maybe", "a bool is true or false");
    // Another game's script warns.
    const sc::ApplyReport other = Run(d, "game MW C\n");
    CHECK_RUN(other);
    CHECK_EQ(other.warnings(), size_t(1));
}

TEST("nfsms: an array may go past the count its definition allows, with a warning; strict stops") {
    auto db = Load();
    Database& d = *db;
    // vehicle's GEAR_RATIO: its definition allows 9 elements.
    const sc::ApplyReport r = Run(d, "add_field vehicle g35 GEAR_RATIO 10\n"
                                     "resize_field vehicle 997gt3rs GEAR_RATIO 11\n"
                                     "append_array vehicle 997gt3rs GEAR_RATIO 0.5\n");
    CHECK_RUN(r);
    CHECK_EQ(r.warnings(), size_t(3));
    CHECK(Messages(r).find("1: GEAR_RATIO has 10 elements, more than the 9 its definition allows") != std::string::npos);
    CHECK_EQ(Own(Col(d, "vehicle", "g35"), "GEAR_RATIO").count, 10u);
    const Attribute& g = Own(Col(d, "vehicle", "997gt3rs"), "GEAR_RATIO");
    CHECK_EQ(g.count, 12u);
    CHECK_EQ(F32(g, 11), 0.5f);
    ns::RunSettings st;
    st.strict = true;
    const sc::ApplyReport s = Run(d, "resize_field vehicle 997gt3rs GEAR_RATIO 13\n", st);
    CHECK(!s.ok);
    CHECK_EQ(Own(Col(d, "vehicle", "997gt3rs"), "GEAR_RATIO").count, 12u);
}

TEST("nfsms: nodes are added, copied, renamed, moved and deleted") {
    auto db = Load(true);
    Database& d = *db;
    Collection& street = Col(d, "pvehicle", "street");
    const std::string script = "add_node pvehicle street ns_new\n"
                               "copy_node pvehicle street ns_copy\n"
                               "copy_node pvehicle street default ns_copy2\n"
                               "rename_node pvehicle ns_copy2 ns_renamed\n"
                               "move_node pvehicle ns_new\n"
                               "add_field pvehicle ns_new HornType\n"
                               "update_field pvehicle ns_new HornType 7\n"
                               "add_field collisionsfx_damage body_panel VolMap 2\n"
                               "resize_field collisionsfx_damage hood VolMap 5\n"
                               "append_array collisionsfx_damage hood VolMap 0x99\n"
                               "delete_field pvehicle ns_copy HornType\n"
                               "copy_fields pvehicle street ns_new base|optional|overwrite\n"
                               "add_overwrite pvehicle ns_renamed\n";
    const sc::ApplyReport r = Run(d, script);
    CHECK_RUN(r);
    Collection& added = Col(d, "pvehicle", "ns_new");
    CHECK(added.parent == nullptr); // moved to the root
    Collection& copy = Col(d, "pvehicle", "ns_copy");
    CHECK(copy.parent == nullptr && copy.vault == street.vault);
    CHECK(!copy.find(StringHash32("HornType")));
    // copy_fields brought street's values over ns_new's.
    CHECK_EQ(int(Own(added, "HornType").data.bytes[0]), int(Own(street, "HornType").data.bytes[0]));
    // CollectionName follows the name.
    const Attribute& cn = Own(copy, "CollectionName");
    CHECK(cn.data.ptrAt(0) && cn.data.ptrAt(0)->text == "ns_copy");
    // add_overwrite replaced ns_renamed with a fresh node at the root.
    Collection& over = Col(d, "pvehicle", "ns_renamed");
    CHECK(over.parent == nullptr);
    CHECK(d.findCollection(StringHash32("pvehicle"), StringHash32("ns_copy2")) == nullptr);
    const Attribute& vm = Own(Col(d, "collisionsfx_damage", "hood"), "VolMap");
    CHECK_EQ(vm.count, 6u);
    CHECK_EQ(U32(vm, 5), 0x99u);
    CHECK_EQ(Own(Col(d, "collisionsfx_damage", "body_panel"), "VolMap").count, 3u); // there already: kept
    // A gameplay node to a vault that doesn't exist yet makes it.
    const sc::ApplyReport g = Run(d, "change_vault gameplay prototype_tracks/autobahn/autobahn_drag_2 ns_vault\n");
    CHECK_RUN(g);
    CHECK_EQ(Col(d, "gameplay", "prototype_tracks/autobahn/autobahn_drag_2").vault->name, std::string("ns_vault"));
    // delete_node takes the children; delete_nodes a whole class.
    CHECK(Run(d, "add_node pvehicle ns_new ns_child\ndelete_node pvehicle ns_new\n").ok);
    CHECK(d.findCollection(StringHash32("pvehicle"), StringHash32("ns_child")) == nullptr);
    CHECK(Run(d, "delete_nodes emittergroup\n").ok);
    CHECK(d.findClass(StringHash32("emittergroup"))->collections.empty());
    // The packs still load.
    for (const auto& pk : d.packs()) {
        if (!pk->dirty) continue;
        Database fresh;
        fresh.addTypeSchema(d.sharedTypeSchema());
        if (pk->path.find("ATTRIBUTES") == std::string::npos) fresh.loadPack(Vanilla("ATTRIBUTES.BIN"));
        fresh.loadPack(pk->path, WritePack(d, *pk));
    }
}

TEST("nfsms: setup choices, line ranges, includes and file copies") {
    namespace fs = std::filesystem;
    auto db = Load();
    Database& d = *db;
    Collection& rev = Col(d, "audio_revlimiter", "default");
    const std::string script = "ui_text Frames\n"                                                  // 1
                               "ui_control checkbox more false \"More frames\"\n"                 // 2
                               "ui_control radiobutton size small \"Small\"\n"                    // 3
                               "ui_control radiobutton size big \"Big\"\n"                        // 4
                               "update_field audio_revlimiter default total_frames_short 1\n"    // 5
                               "ui_option more\n"                                                 // 6
                               "update_field audio_revlimiter default total_frames_short 2\n"    // 7
                               "ui_option more\n"                                                 // 8
                               "ui_option size big\n"                                             // 9
                               "update_field audio_revlimiter default total_frames_long 50\n"    // 10
                               "ui_option size big\n"                                             // 11
                               "script extra.nfsms\n";                                            // 12
    ns::RunSettings st;
    st.readFile = [](const std::string& p) -> std::string {
        CHECK(p.find("extra.nfsms") != std::string::npos);
        return "update_field audio_revlimiter default cutoff_frames_short 9\n";
    };
    const ns::Script s = ns::Parse(script, "setup.nfsms", "/mods/x");
    st.choices = ns::DefaultChoices(s);
    sc::ApplyReport r = ns::Run(d, s, st);
    CHECK_RUN(r);
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 1u); // "more" is off
    CHECK(U32(Own(rev, "total_frames_long")) != 50u);  // no size is chosen
    CHECK_EQ(U32(Own(rev, "cutoff_frames_short")), 9u);
    st.choices.checkboxes["more"] = true;
    st.choices.radios["size"] = "big";
    CHECK(ns::Run(d, s, st).ok);
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 2u);
    CHECK_EQ(U32(Own(rev, "total_frames_long")), 50u);
    // Only line 5: the others stay.
    st.firstLine = st.lastLine = 5;
    CHECK(ns::Run(d, s, st).ok);
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 1u);
    // Line 7 alone runs only while its block's box is ticked.
    st.firstLine = st.lastLine = 7;
    st.choices.checkboxes["more"] = false;
    r = ns::Run(d, s, st);
    CHECK(r.ok);
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 1u);
    CHECK(Messages(r).find("skipped") != std::string::npos); // and says why

    // Files go into the target folder, and only inside it.
    const fs::path tmp = fs::temp_directory_path() / "attribulina_nfsms_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "mod");
    std::ofstream(tmp / "mod" / "readme.txt") << "hi";
    ns::RunSettings files;
    files.targetFolder = (tmp / "game").string();
    files.doFiles = true;
    const ns::Script f = ns::Parse("folder_create TEXTURES\nfile_copy readme.txt TEXTURES/readme.txt\n", "f.nfsms", (tmp / "mod").string());
    CHECK(ns::Run(d, f, files).ok);
    CHECK(fs::exists(tmp / "game" / "TEXTURES" / "readme.txt"));
    const ns::Script bad = ns::Parse("file_copy readme.txt ../outside.txt\n", "f.nfsms", (tmp / "mod").string());
    CHECK_EQ(ns::Run(d, bad, files).errors(), size_t(1));
    CHECK(!fs::exists(tmp / "outside.txt"));
    fs::remove_all(tmp);
}

TEST("nfsms: a diff written as nfsms rebuilds the changed database") {
    auto base = Load();
    auto cur = sc::CloneDatabase(*base);
    const sc::ApplyReport made = sc::Apply(*cur, sc::Parse("attribulina 1\n"
                                                           "create pvehicle/ns_car from pvehicle/street { HornType = 3 }\n"
                                                           "edit audio_revlimiter/default { load_for_short = 0.125\n total_frames_short = 77 }\n"
                                                           "edit collisionsfx_damage/hood { VolMap = [5, 6, 7, 8, 9] }\n"
                                                           "edit effects/default { EmitterQuadratic = (1, 2, 3, 4) }\n"
                                                           "edit fe_global/default { default_car_tuner = pvehicle/street }\n"
                                                           "edit pvehicle/default { BEHAVIOR_MECHANIC_RIGIDBODY = \"changed\" }\n"
                                                           "rename car/gtrproto to gtrproto2\n"
                                                           "move vehicle/gtrproto_track under root\n"
                                                           "edit pvehicle/default { revert BEHAVIOR_MECHANIC_RIGIDBODY }\n"));
    CHECK(made.ok);
    const sc::Script diff = sc::Diff(*base, *cur);
    const std::vector<std::string> lines = ns::FromStatements(*cur, diff.statements);
    std::string text;
    for (const std::string& l : lines) text += l + "\n";
    CHECK(text.find("update_field effects default EmitterQuadratic X 1") != std::string::npos);
    CHECK(text.find("rename_node car gtrproto gtrproto2") != std::string::npos);
    if (text.find("move_node vehicle gtrproto_track\n") == std::string::npos) std::fprintf(stderr, "%s", text.c_str());
    CHECK(text.find("move_node vehicle gtrproto_track\n") != std::string::npos);
    CHECK(text.find("move_node audio_revlimiter") == std::string::npos); // a rename in another class isn't a move
    CHECK(text.find("update_field pvehicle ns_car CollectionName ns_car") != std::string::npos);
    CHECK(text.find("resize_field collisionsfx_damage hood VolMap 5") != std::string::npos);
    CHECK(text.find("delete_field pvehicle default BEHAVIOR_MECHANIC_RIGIDBODY") != std::string::npos);
    auto target = sc::CloneDatabase(*base);
    const sc::ApplyReport r = Run(*target, text);
    if (!r.ok) std::fprintf(stderr, "%s---\n%s", Messages(r).c_str(), text.c_str());
    CHECK_RUN(r);
    const sc::Script again = sc::Diff(*target, *cur);
    if (Changes(again)) std::fprintf(stderr, "%s", sc::Print(again).c_str());
    CHECK_EQ(Changes(again), size_t(0));
    // The line under the cursor names its collection and field.
    uint32_t field = 0;
    CHECK(ns::CollectionOfLine(*cur, ns::ParseLine("update_field audio_revlimiter default load_for_short 1"), &field) ==
          cur->findCollection(StringHash32("audio_revlimiter"), StringHash32("default")));
    CHECK_EQ(field, StringHash32("load_for_short"));
}

TEST("nfsms: a collection written as nfsms is made with add_node and add_field, as NFS-VltEd needs, or edited") {
    auto base = Load();
    auto cur = sc::CloneDatabase(*base);
    CHECK(sc::Apply(*cur, sc::Parse("attribulina 1\n"
                                    "create pvehicle/ns_street from pvehicle/street\n"
                                    "create collisionsfx_damage/ns_hood from collisionsfx_damage/hood { VolMap = [5, 6, 7] }\n"))
              .ok);
    ns::RunSettings st;
    st.names = ns::PropertyNames::Load(test::DataPath("data/nfsms/prostreet_pc.txt"));
    std::vector<const Collection*> made = {&Col(*cur, "pvehicle", "ns_street"), &Col(*cur, "collisionsfx_damage", "ns_hood")};
    const std::vector<std::string> lines = ns::FromStatements(*cur, sc::Export(*cur, made), st.names.get());
    std::string text;
    for (const std::string& l : lines) text += l + "\n";
    // NFS-VltEd's add_node gives a node its layout fields only, and update_field
    // fails on a field the node doesn't have: each other value is added first,
    // an array with its length (no resize_field after it).
    for (const Collection* c : made) {
        const std::string cls = cur->names().format(c->classKey), node = cur->names().format(c->key);
        size_t entries = 0, layout = 0;
        for (const Attribute& a : c->attributes) {
            const std::string field = cur->names().format(a.key);
            int added = -1, set = -1, resized = -1;
            for (size_t i = 0; i < lines.size(); ++i) {
                const ns::Command cmd = ns::ParseLine(lines[i]);
                if (cmd.args.size() < 3 || cmd.args[0] != cls || cmd.args[1] != node) continue;
                const std::string f = cmd.args[2].substr(0, cmd.args[2].find('['));
                if (f != field) continue;
                if (cmd.name == "add_field" && added < 0) added = int(i);
                if ((cmd.name == "update_field" || cmd.name == "update_collection") && set < 0) set = int(i);
                if (cmd.name == "resize_field") resized = int(i);
            }
            if (a.inLayout) {
                ++layout;
                CHECK_EQ(added, -1);
            } else {
                ++entries;
                if (set >= 0 && !(added >= 0 && added < set)) std::fprintf(stderr, "%s/%s %s:\n%s", cls.c_str(), node.c_str(), field.c_str(), text.c_str());
                CHECK(set < 0 || (added >= 0 && added < set));
                CHECK_EQ(resized, -1);
            }
        }
        CHECK(layout > 0 && entries > 0);
    }
    CHECK(text.find("add_node pvehicle ") != std::string::npos);
    CHECK(text.find("add_field collisionsfx_damage ns_hood VolMap 3\n") != std::string::npos);
    CHECK(text.find("update_field collisionsfx_damage ns_hood VolMap[2] 7\n") != std::string::npos);
    // Run where they aren't, it makes them as they are here.
    auto target = sc::CloneDatabase(*base);
    const sc::ApplyReport r = Run(*target, text, st);
    if (!r.ok || !r.messages.empty()) std::fprintf(stderr, "%s---\n%s", Messages(r).c_str(), text.c_str());
    CHECK_RUN(r);
    CHECK(r.messages.empty());
    const sc::Script left = sc::Diff(*target, *cur);
    if (Changes(left)) std::fprintf(stderr, "%s", sc::Print(left).c_str());
    CHECK_EQ(Changes(left), size_t(0));

    // Edits only: update_field lines, which run where the collections are as here.
    sc::ExportSettings edit;
    edit.editOnly = true;
    const Collection* street = &Col(*cur, "pvehicle", "street");
    std::string edits;
    for (const std::string& l : ns::FromStatements(*cur, sc::Export(*cur, {street}, edit), st.names.get())) {
        const ns::Command cmd = ns::ParseLine(l);
        CHECK(cmd.name == "update_field" || cmd.name == "update_collection" || cmd.name == "resize_field");
        edits += l + "\n";
    }
    CHECK(edits.find("update_field pvehicle street HornType ") != std::string::npos);
    auto same = sc::CloneDatabase(*base);
    const sc::ApplyReport e = Run(*same, edits, st);
    CHECK_RUN(e);
    CHECK(e.messages.empty());
    CHECK_EQ(Changes(sc::Diff(*base, *same)), size_t(0));

    // copy_node copies the source's own values: only the others are added.
    const Collection& source = Col(*base, "pvehicle", "street");
    std::string other; // a value street doesn't have
    for (const FieldDef& f : source.cls->fields)
        if (!f.inLayout() && !f.isArray() && !source.find(f.key)) other = base->names().format(f.key);
    CHECK(source.find(StringHash32("HornType")) && !other.empty());
    const sc::Statement copy = sc::Parse("attribulina 1\n"
                                         "create pvehicle/ns_copy from pvehicle/street {\n"
                                         "    HornType = 2\n    " + other + " = 3\n}\n")
                                   .statements.back();
    std::string copied;
    for (const std::string& l : ns::FromStatements(*base, {copy})) copied += l + "\n";
    CHECK(copied.rfind("copy_node pvehicle street ", 0) == 0);
    CHECK(copied.find("add_field pvehicle ns_copy HornType") == std::string::npos);
    CHECK(copied.find("update_field pvehicle ns_copy HornType 2\n") != std::string::npos);
    CHECK(copied.find("add_field pvehicle ns_copy " + other + "\nupdate_field pvehicle ns_copy " + other + " 3\n") != std::string::npos);

    // A car added to a list (an amod append) is Attribulator's append_array and
    // the new last element, so it adds to whatever list the script runs on.
    auto grown = sc::CloneDatabase(*base);
    CHECK(sc::Apply(*grown, sc::Parse("attribulina 2\nedit car_list/all_cars { append Cars = pvehicle/street }\n")).ok);
    std::string appended;
    for (const std::string& l : ns::FromStatements(*grown, sc::Diff(*base, *grown).statements, st.names.get())) appended += l + "\n";
    if (appended.find("append_array car_list all_cars Cars\nupdate_field car_list all_cars Cars[^] Class pvehicle\n"
                      "update_field car_list all_cars Cars[^] Collection street\n") == std::string::npos)
        std::fprintf(stderr, "%s", appended.c_str());
    CHECK(appended.find("append_array car_list all_cars Cars\nupdate_field car_list all_cars Cars[^] Class pvehicle\n"
                        "update_field car_list all_cars Cars[^] Collection street\n") != std::string::npos);
    auto onBase = sc::CloneDatabase(*base);
    const sc::ApplyReport ar = Run(*onBase, appended, st);
    CHECK_RUN(ar);
    CHECK_EQ(Changes(sc::Diff(*onBase, *grown)), size_t(0));
    // And converted to a mod script, it is an append again.
    ns::RunSettings conv = st;
    auto scratch = sc::CloneDatabase(*base);
    const std::string amod = sc::Print(ns::ToModScript(*scratch, ns::Parse("append_array car_list all_cars Cars\n", "a.nfsms"), conv));
    CHECK(amod.find("append Cars\n") != std::string::npos);
}

TEST("nfsms: NFS-VltEd compatible runs read values as NFS-VltEd 4.6 does") {
    auto base = Load();
    ns::RunSettings ext;
    ext.names = ns::PropertyNames::Load(test::DataPath("data/nfsms/prostreet_pc.txt"));
    ns::RunSettings vlt = ext;
    vlt.vltEd = true;
    auto fresh = [&]() { return sc::CloneDatabase(*base); };
    auto U8 = [](const Attribute& a, uint32_t i, uint32_t off) { return unsigned(a.data.bytes[i * a.elemSize + off]); };

    // FEPartData's 4 is an int32 over tier, drift, drag and grip in NFS-VltEd.
    {
        auto db = fresh();
        const std::string line = "update_field part_fedata generic_spoiler_evo partData[0] 4 16777474\n"; // 0x01000102
        CHECK(!Run(*db, line, ext).ok || Run(*db, line, ext).errors() > 0);
        auto d2 = fresh();
        const sc::ApplyReport r = Run(*d2, line, vlt);
        CHECK_RUN(r);
        CHECK_EQ(r.errors(), size_t(0));
        const Attribute& pd = Own(Col(*d2, "part_fedata", "generic_spoiler_evo"), "partData");
        CHECK_EQ(U8(pd, 0, 12), 2u); // tier
        CHECK_EQ(U8(pd, 0, 13), 1u); // drift
        CHECK_EQ(U8(pd, 0, 14), 0u); // drag
        CHECK_EQ(U8(pd, 0, 15), 1u); // grip
        // And the editor writes that word whole for NFS-VltEd.
        const sc::Statement e = sc::ExportValue(*d2, Col(*d2, "part_fedata", "generic_spoiler_evo"), StringHash32("partData"));
        std::string text;
        for (const std::string& l : ns::FromStatements(*d2, {e}, ext.names.get(), true)) text += l + "\n";
        if (text.find("update_field part_fedata generic_spoiler_evo partData[0] 4 16777474\n") == std::string::npos) std::fprintf(stderr, "%s", text.c_str());
        CHECK(text.find("update_field part_fedata generic_spoiler_evo partData[0] 4 16777474\n") != std::string::npos);
        auto d3 = fresh();
        CHECK_RUN(Run(*d3, text, vlt));
        CHECK_EQ(Changes(sc::Diff(*d3, *d2)), size_t(0));
    }

    // A layout array holds its capacity: writes past the count wait for a
    // resize_field, and a shrink keeps the values for a later growth.
    {
        auto db = fresh();
        const std::string script = "update_field table_data steering_input_remap_table data[30] 0.5\n"
                                   "resize_field table_data steering_input_remap_table data 31\n"
                                   "resize_field table_data steering_input_remap_table data 27\n"
                                   "resize_field table_data steering_input_remap_table data 31\n";
        CHECK_EQ(Run(*fresh(), script, ext).errors(), size_t(1));
        const sc::ApplyReport r = Run(*db, script, vlt);
        CHECK_RUN(r);
        CHECK_EQ(r.errors(), size_t(0));
        const Attribute& data = Own(Col(*db, "table_data", "steering_input_remap_table"), "data");
        CHECK_EQ(data.count, 31u);
        CHECK(F32(data, 30) == 0.5f);
        CHECK(F32(data, 27) == 0.0f);
        CHECK(!Run(*db, "resize_field table_data steering_input_remap_table data 101\n", vlt).errors() == 0);
    }

    // copy_fields: overwrite replaces the target's own values that the source has
    // (both readings); a value command on an inherited field fails in NFS-VltEd.
    for (const ns::RunSettings* st : {&ext, &vlt}) {
        auto db = fresh();
        const sc::ApplyReport r = Run(*db, "copy_node vehicle dbr9_track dbr9 ns_a\n"
                                           "copy_fields vehicle zonda_track_highend ns_a base|overwrite\n", *st);
        CHECK_RUN(r);
        CHECK_EQ(Own(Col(*db, "vehicle", "ns_a"), "TOE").count, Own(Col(*db, "vehicle", "zonda_track_highend"), "TOE").count);
        CHECK(!Run(*db, "copy_fields vehicle zonda_track_highend ns_a nothing\n", *st).errors() == 0);
    }
    {
        auto db = fresh();
        CHECK_EQ(Run(*db, "update_field vehicle ccx_draghpfall STEERING_SPEED[0] 1\n", vlt).errors(), size_t(1));
        CHECK_EQ(Run(*db, "update_field vehicle ccx_draghpfall STEERING_SPEED[0] 1\n", ext).errors(), size_t(0));
    }

    // add_field starts as the class's default collection has it: its values and
    // its count, the rest of the slots held.
    {
        auto db = fresh();
        CHECK_RUN(Run(*db, "add_field vehicle ccx_draghpfall STEERING_SPEED 12\n"
                           "update_field vehicle ccx_draghpfall STEERING_SPEED[11] 9\n", vlt));
        const Attribute& a = Own(Col(*db, "vehicle", "ccx_draghpfall"), "STEERING_SPEED");
        const Attribute& def = Own(Col(*db, "vehicle", "default"), "STEERING_SPEED");
        CHECK_EQ(a.count, def.count);
        CHECK(F32(a, 3) == F32(def, 3));
        CHECK_EQ(Run(*db, "add_field vehicle ccx_draghpfall STEERING_SPEED 12\n", vlt).errors(), size_t(1)); // it has it now
        // A slot past the count comes in with a resize_field later in the run.
        auto held = fresh();
        CHECK_RUN(Run(*held, "add_field vehicle ccx_draghpfall STEERING_SPEED 12\n"
                             "update_field vehicle ccx_draghpfall STEERING_SPEED[11] 9\n"
                             "resize_field vehicle ccx_draghpfall STEERING_SPEED 12\n", vlt));
        CHECK(F32(Own(Col(*held, "vehicle", "ccx_draghpfall"), "STEERING_SPEED"), 11) == 9.0f);
        auto e = fresh();
        CHECK_RUN(Run(*e, "add_field vehicle ccx_draghpfall STEERING_SPEED 12\n", ext));
        CHECK_EQ(Own(Col(*e, "vehicle", "ccx_draghpfall"), "STEERING_SPEED").count, 12u);
        CHECK(F32(Own(Col(*e, "vehicle", "ccx_draghpfall"), "STEERING_SPEED"), 3) == 0.0f);
    }

    // rename_node to a hash (both readings), and MODEL's Collision and Visual alone.
    {
        auto db = fresh();
        CHECK_RUN(Run(*db, "copy_node pvehicle street ns_r\nrename_node pvehicle ns_r 0x12345678\n", ext));
        CHECK(db->findCollection(StringHash32("pvehicle"), 0x12345678u) != nullptr);
        Collection& r = *db->findCollection(StringHash32("pvehicle"), 0x12345678u);
        const uint32_t hash = U32(Own(r, "MODEL"));
        CHECK_RUN(Run(*db, "update_field pvehicle 0x12345678 MODEL Visual NS_TEXT\n", vlt));
        CHECK_EQ(U32(Own(r, "MODEL")), hash);
        CHECK(Own(r, "MODEL").data.ptrAt(4) && Own(r, "MODEL").data.ptrAt(4)->text == "NS_TEXT");
        CHECK_RUN(Run(*db, "update_field pvehicle 0x12345678 MODEL Collision NS_OTHER\n", vlt));
        CHECK_EQ(U32(Own(r, "MODEL")), StringHash32("NS_OTHER"));
        CHECK(Own(r, "MODEL").data.ptrAt(4)->text == "NS_TEXT");
    }

    // A curve point leaves Y2 as it is; a list resized keeps the point count the
    // file has, so the game reads X's last point from Y's first.
    {
        auto db = fresh();
        Collection& bmw = Col(*db, "vehicle", "bmwm3e92_drifthigh");
        const CurveValue before = ReadCurve(Own(bmw, "TORQUE"), 0, Endian::Little);
        CHECK_RUN(Run(*db, "update_collection vehicle bmwm3e92_drifthigh TORQUE YValues 3 Value 0.5\n", vlt));
        CurveValue c = ReadCurve(Own(bmw, "TORQUE"), 0, Endian::Little);
        CHECK(c.y[3] == 0.5f);
        CHECK(c.y2 == before.y2);
        CHECK_RUN(Run(*db, "resize_collection vehicle bmwm3e92_drifthigh TORQUE XValues 8\n"
                           "resize_collection vehicle bmwm3e92_drifthigh TORQUE YValues 8\n", vlt));
        c = ReadCurve(Own(bmw, "TORQUE"), 0, Endian::Little);
        CHECK_EQ(c.x.size(), before.x.size());
        CHECK(c.x[8] == c.y[0]);           // X's last from Y's first
        CHECK(c.y.back() == before.y2[0]); // Y's last from Y2's first
        auto e = fresh();
        CHECK_RUN(Run(*e, "update_collection vehicle bmwm3e92_drifthigh TORQUE YValues 3 Value 0.5\n", ext));
        CHECK(ReadCurve(Own(Col(*e, "vehicle", "bmwm3e92_drifthigh"), "TORQUE"), 0, Endian::Little).y2 != before.y2);
    }

    // What the editor writes for NFS-VltEd: no [^] or append_array, an array
    // given its length after add_field, and a value a collection only inherited
    // added first.
    {
        auto db = fresh();
        auto before = fresh();
        CHECK(sc::Apply(*db, sc::Parse("attribulina 2\nedit car_list/all_cars { append Cars = pvehicle/street }\n"
                                       "edit vehicle/ccx_draghpfall { STEERING_SPEED[0] = 1 }\n")).ok);
        std::string text;
        for (const std::string& l : ns::FromStatements(*db, sc::Diff(*before, *db).statements, ext.names.get(), true, before.get()))
            text += l + "\n";
        CHECK(text.find("[^]") == std::string::npos);
        CHECK(text.find("append_array") == std::string::npos);
        const uint32_t n = Own(Col(*db, "car_list", "all_cars"), "Cars").count;
        CHECK(text.find("resize_field car_list all_cars Cars " + std::to_string(n) + "\n") != std::string::npos);
        CHECK(text.find("update_field car_list all_cars Cars[" + std::to_string(n - 1) + "] Collection street\n") != std::string::npos);
        CHECK(text.find("add_field vehicle ccx_draghpfall STEERING_SPEED 10\nresize_field vehicle ccx_draghpfall STEERING_SPEED 10\n") != std::string::npos);
        auto again = fresh();
        const sc::ApplyReport r = Run(*again, text, vlt);
        if (r.errors()) std::fprintf(stderr, "%s---\n%s", Messages(r).c_str(), text.c_str());
        CHECK_EQ(r.errors(), size_t(0));
        CHECK_EQ(Changes(sc::Diff(*again, *db)), size_t(0));
    }
}

TEST("nfsms: the Shifted mod (nfsms/Shifted, when present) runs NFS-VltEd compatible with no failed line") {
    namespace fs = std::filesystem;
    const std::string mod = test::DataPath("nfsms/Shifted/VLT Data Install.nfsms");
    if (!fs::exists(fs::path(mod))) {
        std::printf("        (skipped: no %s)\n", mod.c_str());
        return;
    }
    auto db = Load(true);
    std::ifstream in(fs::path(mod), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const ns::Script s = ns::Parse(ss.str(), "VLT Data Install.nfsms", fs::path(mod).parent_path().string());
    ns::RunSettings st;
    st.names = ns::PropertyNames::Load(test::DataPath("data/nfsms/prostreet_pc.txt"));
    st.vltEd = true;
    st.choices = ns::DefaultChoices(s);
    const sc::ApplyReport r = ns::Run(*db, s, st);
    if (r.errors()) std::fprintf(stderr, "%s", Messages(r).substr(0, 2000).c_str());
    CHECK_EQ(r.errors(), size_t(0));
    CHECK(r.valuesSet > 50000);
    // Read with Attribulina's own words, the lines that rely on NFS-VltEd's fail.
    auto other = Load(true);
    ns::RunSettings ext = st;
    ext.vltEd = false;
    CHECK(ns::Run(*other, s, ext).errors() > 400);
}

TEST("nfsms: NFS-VltEd compatible runs set Undercover's curves in structs, vinyl layers' words, a u32 over a float and a reference's pointer word") {
    const std::string dir = test::DataPath("attrib/databases/pc/undercover-vanilla");
    if (!std::filesystem::exists(std::filesystem::path(dir))) return;
    auto base = LoadFolder(dir);
    ns::RunSettings vlt;
    vlt.names = ns::PropertyNames::Load(test::DataPath("data/nfsms/undercover_pc.txt"));
    vlt.vltEd = true;
    auto db = sc::CloneDatabase(*base);
    Database& d = *db;
    const sc::ApplyReport r = Run(d,
                                  // AI::GlueCurve holds two curves, Easy and Hard.
                                  "update_field glue default SpreadOverRace Easy_MinX 51500\n"
                                  "update_collection glue default SpreadOverRace Easy_Y2Values 1 Value 1.5\n"
                                  "update_collection glue default SpreadOverRace Hard_YValues 2 Value 0.25\n"
                                  // VinylLayer's 2 to 7 are u32s over its transform and colors[4].
                                  "update_field presetride nfsdotcom VinylLayers[0] 2 0x00ba0001\n"
                                  "update_field presetride nfsdotcom VinylLayers[0] 3 0x51c2ffde\n"
                                  "update_field presetride nfsdotcom VinylLayers[0] 4 0x00000030\n"
                                  "update_field presetride nfsdotcom VinylLayers[0] 5 0xff000008\n"
                                  "update_field presetride nfsdotcom VinylLayers[0] 6 0x00ff0000\n"
                                  "update_field presetride nfsdotcom VinylLayers[0] 7 0x0000ff00\n"
                                  // PresetRidePaint's 6 is a u32 over our float mSaturation, its 4 the group's pointer word.
                                  "update_field car por_911_tur_06 stockPaint[0] 6 0x3f800000\n"
                                  "update_field car por_911_tur_06 stockPaint[0] 4 5\n"
                                  "update_field physics_tuning_slider tires tunings[1] 3 7\n",
                                  vlt);
    CHECK_RUN(r);
    CHECK_EQ(r.errors(), size_t(0));

    Collection& glue = Col(d, "glue", "default");
    auto curve = [&](Collection& c, const char* member) {
        sc::FieldPath p;
        p.field = "SpreadOverRace";
        p.steps.push_back({member, -1});
        uint32_t off = 0;
        const DataBlock* b = sc::BlockAt(d, c, p, &off);
        CHECK(b != nullptr);
        return b ? ReadCurve(*b, off, Endian::Little, 48) : CurveValue{};
    };
    const CurveValue easyBefore = curve(Col(*base, "glue", "default"), "Easy");
    const CurveValue easy = curve(glue, "Easy"), hard = curve(glue, "Hard");
    CHECK(easy.xMin == 51500.0f);
    CHECK(easy.y2[1] == 1.5f);
    CHECK(easy.y2[2] == easyBefore.y2[2]); // as given: NFS-VltEd never works Y2 out
    CHECK(hard.y[2] == 0.25f);

    const Attribute& vinyl = Own(Col(d, "presetride", "nfsdotcom"), "VinylLayers");
    const uint32_t words[] = {0x00ba0001, 0x51c2ffde, 0x00000030, 0xff000008, 0x00ff0000, 0x0000ff00};
    for (int i = 0; i < 6; ++i) CHECK_EQ(U32(vinyl, 0, 4 + 4 * i), words[i]);

    const Attribute& paint = Own(Col(d, "car", "por_911_tur_06"), "stockPaint");
    CHECK(F32(paint, 0, 20) == 1.0f);
    CHECK_EQ(U32(paint, 0, 12), 5u);
    CHECK_EQ(U32(Own(Col(d, "physics_tuning_slider", "tires"), "tunings"), 1, 8), 7u);

    // The editor writes them back in those words, and they read back the same
    // (a reference's two keys: the pointer word, which the game fills in, back to 0).
    CHECK_RUN(Run(d, "update_field car por_911_tur_06 stockPaint[0] 4 0\n", vlt));
    std::vector<sc::Statement> values;
    values.push_back(sc::ExportValue(d, glue, StringHash32("SpreadOverRace")));
    values.push_back(sc::ExportValue(d, Col(d, "presetride", "nfsdotcom"), StringHash32("VinylLayers")));
    values.push_back(sc::ExportValue(d, Col(d, "car", "por_911_tur_06"), StringHash32("stockPaint")));
    std::string text;
    for (const std::string& l : ns::FromStatements(d, values, vlt.names.get(), true)) text += l + "\n";
    for (const char* want : {"update_collection glue default SpreadOverRace Easy_Y2Values 1 Value 1.5\n",
                             "update_field presetride nfsdotcom VinylLayers[0] 3 0x51C2FFDE\n",
                             "update_field car por_911_tur_06 stockPaint[0] 6 0x3F800000\n"}) {
        if (text.find(want) == std::string::npos) std::fprintf(stderr, "no %s in\n%s", want, text.c_str());
        CHECK(text.find(want) != std::string::npos);
    }
    auto again = sc::CloneDatabase(*base);
    const sc::ApplyReport back = Run(*again, text, vlt);
    if (back.errors()) std::fprintf(stderr, "%s", Messages(back).c_str());
    CHECK_EQ(back.errors(), size_t(0));
    sc::DiffSettings only;
    only.classes = {StringHash32("glue"), StringHash32("presetride"), StringHash32("car")};
    const sc::Script left = sc::Diff(*again, d, only);
    if (Changes(left)) std::fprintf(stderr, "%s", sc::Print(left).c_str());
    CHECK_EQ(Changes(left), size_t(0));
}

TEST("nfsms: a script or file named in another case is found, as on Windows") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "attribulina_nfsms_case";
    fs::remove_all(dir);
    fs::create_directories(dir / "Sub");
    std::ofstream(dir / "Sub" / "inc.nfsms") << "update_field audio_revlimiter default total_frames_long 47\n";
    CHECK_EQ(ns::ScriptFile(dir.string(), "sub\\INC.nfsms"), (dir / "Sub" / "inc.nfsms").string());
    auto db = Load();
    const ns::Script s = ns::Parse("version 4.6\nscript\"SUB\\Inc.nfsms\"\n", "main.nfsms", dir.string());
    const sc::ApplyReport r = ns::Run(*db, s, {});
    CHECK_RUN(r);
    CHECK_EQ(U32(Own(Col(*db, "audio_revlimiter", "default"), "total_frames_long")), 47u);
    fs::remove_all(dir);
}

TEST("nfsms: NFS Undercover Reformed's vault installer (nfsms/Part 2 (Vault), when present) runs NFS-VltEd compatible with no failed line") {
    namespace fs = std::filesystem;
    const std::string mod = test::DataPath("nfsms/Part 2 (Vault)/Installer.nfsms");
    const std::string dir = test::DataPath("attrib/databases/pc/undercover-vanilla");
    if (!fs::exists(fs::path(mod)) || !fs::exists(fs::path(dir))) {
        std::printf("        (skipped: no %s)\n", mod.c_str());
        return;
    }
    auto base = LoadFolder(dir);
    std::ifstream in(fs::path(mod), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const ns::Script s = ns::Parse(ss.str(), "Installer.nfsms", fs::path(mod).parent_path().string());
    ns::RunSettings st;
    st.names = ns::PropertyNames::Load(test::DataPath("data/nfsms/undercover_pc.txt"));
    st.vltEd = true;
    st.choices = ns::DefaultChoices(s);
    auto db = sc::CloneDatabase(*base);
    const sc::ApplyReport r = ns::Run(*db, s, st);
    if (r.errors()) std::fprintf(stderr, "%s", Messages(r).substr(0, 2000).c_str());
    CHECK_EQ(r.errors(), size_t(0));
    CHECK(r.valuesSet > 70000);
    // Read with Attribulina's own words, its curves' Y2 lines, vinyl layers and
    // the slots past an array's count fail.
    ns::RunSettings ext = st;
    ext.vltEd = false;
    CHECK(ns::Run(*sc::CloneDatabase(*base), s, ext).errors() > 1000);
}

TEST("nfsms: World's scripts edit its packs and event vaults, and record back as nfsms") {
    auto base = LoadFolder(test::DataPath("attrib/databases/pc/world-final"));
    CHECK_EQ(ns::GameCode(*base), std::string("W"));
    ns::RunSettings st;
    st.names = ns::PropertyNames::Load(test::DataPath("data/nfsms/world_pc.txt"));
    auto db = sc::CloneDatabase(*base);
    Database& d = *db;
    const sc::ApplyReport r = Run(d,
                                  "version 4.6\n"
                                  "game W\n"
                                  "update_field ecar default IntegratedKitWheelOffsetFront[0] 2 CROWNCOP_KIT12_INTEGRATED_KIT\n"
                                  "update_field ecar default IntegratedKitWheelOffsetFront[0] 3 45\n"
                                  "update_field virtualitem 0x38780AC3 longdescription LocalizedString GM_TEST_STRING\n"
                                  "update_field partcollections 0032 hat_bonus[0] Acceleration 13\n"
                                  "update_field gameplay race_bin_01/1_2_1_sprint DifficultyLevel 90\n"
                                  "update_field gameplay race_bin_01/1_2_1_sprint EngageTriggerPosition X 6200.5\n"
                                  "copy_node gameplay race_bin_01/1_2_1_sprint/engage race_bin_01/1_2_1_sprint race_bin_01/1_2_1_sprint/engage2\n",
                                  st);
    CHECK_RUN(r);
    Attribute& kit = Own(Col(d, "ecar", "default"), "IntegratedKitWheelOffsetFront");
    const PtrSlot* text = kit.data.ptrAt(4);
    CHECK(text && text->text == "CROWNCOP_KIT12_INTEGRATED_KIT");
    CHECK_EQ(U32(kit, 0, 0), StringHash32("CROWNCOP_KIT12_INTEGRATED_KIT"));
    CHECK_EQ(U32(kit, 0, 8), 45u);
    Collection* item = d.findCollection(StringHash32("virtualitem"), 0x38780AC3u); // unnamed: its key
    CHECK(item);
    const PtrSlot* desc = item ? Own(*item, "longdescription").data.ptrAt(0) : nullptr;
    CHECK(desc && desc->text == "GM_TEST_STRING");
    CHECK_EQ(U32(Own(Col(d, "partcollections", "0032"), "hat_bonus")), 13u);
    // The event and its copied child stay in the event vault, whose file is the one changed.
    Collection& race = Col(d, "gameplay", "race_bin_01/1_2_1_sprint");
    CHECK_EQ(U32(Own(race, "DifficultyLevel")), 90u);
    CHECK(F32(Own(race, "EngageTriggerPosition")) == 6200.5f);
    Collection& copy = Col(d, "gameplay", "race_bin_01/1_2_1_sprint/engage2");
    CHECK(copy.vault == race.vault && race.vault->name == "1_2_1_sprint");
    CHECK(race.vault->pack->dirty);
    CHECK(race.vault->pack->format == PackFormat::VaultFile);
    for (const auto& p : d.packs())
        if (p->dirty)
            CHECK(p.get() == race.vault->pack || p->path.find("attributes.bin") != std::string::npos ||
                  p->path.find("commerce.bin") != std::string::npos);

    // What changed, written as nfsms, makes the same database from the files.
    const std::vector<std::string> lines = ns::FromStatements(d, sc::Diff(*base, d).statements);
    std::string again;
    for (const std::string& l : lines) again += l + "\n";
    auto target = sc::CloneDatabase(*base);
    const sc::ApplyReport r2 = Run(*target, again, st);
    if (!r2.ok) std::fprintf(stderr, "%s---\n%s", Messages(r2).c_str(), again.c_str());
    CHECK_RUN(r2);
    const sc::Script left = sc::Diff(*target, d);
    if (Changes(left)) std::fprintf(stderr, "%s", sc::Print(left).c_str());
    CHECK_EQ(Changes(left), size_t(0));

    // The betas are NFS-VltEd's WB, and a script for W warns there.
    auto beta = LoadFolder(test::DataPath("attrib/databases/pc/world-2009-beta"));
    CHECK_EQ(ns::GameCode(*beta), std::string("WB"));
    ns::RunSettings wb;
    wb.names = ns::PropertyNames::Load(test::DataPath("data/nfsms/world_beta_pc.txt"));
    const sc::ApplyReport rb = Run(*beta, "version 4.6\ngame WB\nupdate_field combat_abilities sprinter abilities[0] 2 25\n", wb);
    CHECK_RUN(rb);
    CHECK(rb.messages.empty());
    CHECK(F32(Own(Col(*beta, "combat_abilities", "sprinter"), "abilities"), 0, 4) == 25.0f);
    const sc::ApplyReport rw = Run(*beta, "version 4.6\ngame W\n", wb);
    CHECK(Messages(rw).find("the script is for W; this database is WB") != std::string::npos);
}

TEST("nfsms: a line that fails is reported and the next one runs, as in NFS-VltEd") {
    auto db = Load();
    Database& d = *db;
    Collection& rev = Col(d, "audio_revlimiter", "default");
    const std::string script = "version 4.6\n"
                               "update_field audio_revlimiter default total_frames_short 11\n" // 2
                               "resize_field pvehicle\n"                                        // 3: too few parameters
                               "update_field pvehicle nope HornType 1\n"                        // 4: no such node
                               "update_field audio_revlimiter default total_frames_long 12\n"  // 5
                               "ui_control checkbox broken maybe \"Broken\"\n";                 // 6: bad setup line
    sc::ApplyReport r = Run(d, script);
    CHECK(r.ok);
    CHECK_EQ(r.errors(), size_t(3));
    std::vector<int> lines;
    for (const sc::Message& m : r.messages)
        if (m.level == sc::Message::Level::Error) lines.push_back(m.pos.line);
    CHECK(lines == (std::vector<int>{3, 4, 6}));
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 11u);
    CHECK_EQ(U32(Own(rev, "total_frames_long")), 12u);
    // Strict stops at the first one.
    ns::RunSettings strict;
    strict.strict = true;
    r = Run(d, "update_field audio_revlimiter default total_frames_short 21\ndelete_node pvehicle nope\nupdate_field audio_revlimiter default total_frames_long 22\n",
            strict);
    CHECK(!r.ok);
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 21u);
    CHECK_EQ(U32(Own(rev, "total_frames_long")), 12u);
}

TEST("nfsms: a field the schema gives an enum (light_flares_cg's flare_texture) is named in scripts and a number in nfsms") {
    const std::string dir = test::DataPath("attrib/databases/pc/carbon-vanilla");
    if (!std::filesystem::exists(dir)) return;
    auto db = LoadFolder(dir);
    Collection& c = Col(*db, "light_flares_cg", "headlight_inner");
    const std::string text = sc::Print(sc::NewScript(*db, sc::Export(*db, {&c})));
    CHECK(text.find("flare_texture = ESLF_TEX_HEADLIGHT_OUTER") != std::string::npos);
    // A script sets it by name or by number.
    const sc::ApplyReport byName =
        sc::Apply(*db, sc::Parse("attribulina 1\nedit light_flares_cg/headlight_inner { flare_texture = ESLF_TEX_LAMPPOSTFLARE }\n", "a.amod"));
    CHECK(byName.ok);
    CHECK_EQ(U32(Own(c, "flare_texture")), 4u);
    const sc::ApplyReport byNumber = sc::Apply(*db, sc::Parse("attribulina 1\nedit light_flares_cg/headlight_inner { flare_texture = 3 }\n", "a.amod"));
    CHECK(byNumber.ok);
    CHECK_EQ(U32(Own(c, "flare_texture")), 3u);
    // NFS-VltEd reads it as the UInt32 the type table says: nfsms lines take
    // the number, and the recorder writes it.
    CHECK_RUN(Run(*db, "update_field light_flares_cg headlight_inner flare_texture 2"));
    CHECK_EQ(U32(Own(c, "flare_texture")), 2u);
    const sc::Script edits = sc::Parse("attribulina 1\nedit light_flares_cg/headlight_inner { flare_texture = ESLF_TEX_CATSEYE }\n", "rec.amod");
    const std::vector<std::string> lines = ns::FromStatements(*db, edits.statements);
    CHECK(std::find(lines.begin(), lines.end(), "update_field light_flares_cg headlight_inner flare_texture 3") != lines.end());
    if (std::find(lines.begin(), lines.end(), "update_field light_flares_cg headlight_inner flare_texture 3") == lines.end())
        for (const std::string& l : lines) std::fprintf(stderr, "    recorded: %s\n", l.c_str());
    // A number no name has (for code changed to have more textures) goes in,
    // and comes out as that number.
    const sc::ApplyReport past = sc::Apply(*db, sc::Parse("attribulina 1\nedit light_flares_cg/headlight_inner { flare_texture = 12 }\n", "a.amod"));
    CHECK(past.ok);
    CHECK_EQ(U32(Own(c, "flare_texture")), 12u);
    CHECK(sc::Print(sc::NewScript(*db, sc::Export(*db, {&c}))).find("flare_texture = 12") != std::string::npos);
    CHECK_RUN(Run(*db, "update_field light_flares_cg headlight_inner flare_texture 13"));
    CHECK_EQ(U32(Own(c, "flare_texture")), 13u);
    const std::vector<std::string> pastLines =
        ns::FromStatements(*db, sc::Parse("attribulina 1\nedit light_flares_cg/headlight_inner { flare_texture = 12 }\n", "rec.amod").statements);
    CHECK(std::find(pastLines.begin(), pastLines.end(), "update_field light_flares_cg headlight_inner flare_texture 12") != pastLines.end());
    // Most Wanted's build has no cat's eye texture.
    const std::string mwDir = test::DataPath("attrib/databases/pc/mw-vanilla");
    if (!std::filesystem::exists(mwDir)) return;
    auto mw = LoadFolder(mwDir);
    const sc::ApplyReport none =
        sc::Apply(*mw, sc::Parse("attribulina 1\nedit light_flares_cg/headlight_outer { flare_texture = ESLF_TEX_CATSEYE }\n", "a.amod"));
    CHECK(!none.ok);
    CHECK(Messages(none).find("has no value ESLF_TEX_CATSEYE") != std::string::npos);
}
