// Games and platforms (core/Game.h): target ids, what the files say they
// are, and the type schema a target picks.

#include "Test.h"

#include "core/Database.h"
#include "core/Game.h"
#include "core/Schema.h"

#include <filesystem>
#include <map>

using namespace attrib;

namespace {
std::string Db(const std::string& folder) { return test::DataPath("attrib/databases/" + folder); }

// A pack's bytes with the first vault's Vers value changed: files from a
// build whose value isn't known.
Bytes WithUnknownVers(const std::string& file) {
    Bytes b = ReadFile(file);
    const uint32_t vltOffset = load32(b.data() + 0x10 + 16, Endian::Little);
    for (int i = 8; i < 16; ++i) b[vltOffset + i] ^= 0x5A;
    return b;
}
} // namespace

TEST("game: target ids name a game and a platform, and read back") {
    for (const GameInfo& g : Games())
        for (const PlatformInfo& p : Platforms()) {
            const Target t{g.game, p.platform};
            CHECK(ParseTarget(t.id()) == t);
        }
    CHECK_EQ((Target{Game::ProStreet, Platform::Xbox360}).id(), std::string("prostreet_x360"));
    CHECK_EQ((Target{Game::ProStreet, Platform::PS2}).name(), std::string("Pro Street on PS2"));
    CHECK(ParseTarget("ProStreet_PC") == (Target{Game::ProStreet, Platform::PC}));
    CHECK(!ParseTarget("prostreet"));
    CHECK(!ParseTarget("prostreet_dreamcast"));
    CHECK(!ParseTarget("nfs_pc"));
    CHECK((Target{}).id().empty());
    // What each game came out on.
    CHECK(ReleasedOn(Game::ProStreet, Platform::Wii));
    CHECK(!ReleasedOn(Game::ProStreet, Platform::GameCube));
    CHECK(!ReleasedOn(Game::World, Platform::PS3));
    CHECK(ReleasedOn(Game::Nitro, Platform::Wii) && !ReleasedOn(Game::Nitro, Platform::PC));
    CHECK(SupportedOn(Game::ProStreet, Platform::PS2));
    CHECK(SupportedOn(Game::Carbon, Platform::GameCube));
    CHECK(SupportedOn(Game::Carbon, Platform::Xbox360)); // from a late demo
    CHECK(!SupportedOn(Game::Carbon, Platform::Xbox));    // no files seen
    CHECK(SupportedOn(Game::Undercover, Platform::Wii));
    CHECK(SupportedOn(Game::Undercover, Platform::PC) && SupportedOn(Game::Undercover, Platform::Xbox360));
    CHECK(!SupportedOn(Game::Undercover, Platform::PS3)); // no files seen; it may share the 360's Vers
    CHECK(SupportedOn(Game::Undercover, Platform::PS2)); // the Wii's game; no files seen
    CHECK(SupportedOn(Game::MostWanted, Platform::Xbox) && SupportedOn(Game::MostWanted, Platform::GameCube));
    CHECK(SupportedOn(Game::World, Platform::PC)); // the final build and its betas, one schema
    CHECK(!IsWorldBetaVersion(0x6838F8C1836A18DEull) && IsWorldBetaVersion(0x2EDE8B2CB6712231ull));
    CHECK(!SupportedAnywhere(Game::Nitro));
}

TEST("game: every database on disk says what it is, the early Most Wanted demos too") {
    const std::map<std::string, std::pair<std::string, bool>> want = {
        {"pc/prostreet-vanilla", {"prostreet_pc", true}},     {"360/prostreet-vanilla", {"prostreet_x360", true}},
        {"ps2/prostreet-vanilla", {"prostreet_ps2", true}},   {"pc/carbon-vanilla", {"carbon_pc", true}},
        {"gamecube/carbon-vanilla", {"carbon_gamecube", true}},
        {"ps2/carbon-vanilla", {"carbon_ps2", true}},         {"ps2/carbon-demo", {"carbon_ps2", true}},
        {"pc/undercover-vanilla", {"undercover_pc", true}},   {"360/undercover-vanilla", {"undercover_x360", true}},
        {"pc/world-final", {"world_pc", true}},               {"pc/world-2010-beta", {"world_pc", true}},
        {"pc/world-2009-beta", {"world_pc", true}},           {"pc/mw-vanilla", {"mw_pc", true}},
        {"ps2/mw-beta", {"mw_ps2", true}},                    {"360/mw-beta", {"mw_x360", true}},
        {"gamecube/mw-vanilla", {"mw_gamecube", true}},       {"xbox/mw-vanilla", {"mw_xbox", true}},
        {"wii/prostreet-vanilla", {"prostreet_wii", true}},   {"wii/undercover-vanilla", {"undercover_wii", true}},
        {"wii/nitro-vanilla", {"nitro_wii", true}},           {"360/carbon-latedemo", {"carbon_x360", true}},
        {"xbox/mw-early-oxm-betademo", {"mw_xbox", true}},    {"ps2/mw-early-opm-betademo", {"mw_ps2", true}},
        // Carbon's PS3 and Wii files share a Vers value and byte order; attributes.bin's classes tell them apart.
        {"ps3/carbon-vanilla", {"carbon_ps3", true}},         {"ps3/carbon-latebeta", {"carbon_ps3", true}},
        {"wii/carbon-vanilla", {"carbon_wii", true}},
    };
    size_t checked = 0;
    for (const auto& [folder, expect] : want) {
        if (!std::filesystem::exists(Db(folder))) continue;
        const Detection d = DetectTarget(Db(folder));
        if (d.target.id() != expect.first || d.certain != expect.second)
            std::printf("        %s: %s%s (%s)\n", folder.c_str(), d.target.id().c_str(), d.certain ? "" : "?", d.reason.c_str());
        CHECK_EQ(d.target.id(), expect.first);
        CHECK_EQ(d.certain, expect.second);
        ++checked;
    }
    CHECK(checked >= 3);
    // The early Most Wanted demos key everything with StringHash64; their
    // gameplay.bin alone (the same file in both) holds no type table.
    for (const char* demo : {"ps2/mw-early-opm-betademo", "xbox/mw-early-oxm-betademo"})
        if (std::filesystem::exists(Db(demo))) {
            CHECK(DetectTarget(Db(demo)).reason.find("64-bit keys") != std::string::npos);
            const Detection g = DetectTarget("GAMEPLAY.BIN", ReadFile(Db(demo) + "/GAMEPLAY.BIN"));
            CHECK(!g.certain && g.target.game == Game::MostWanted && g.reason.find("keys are 64-bit") != std::string::npos);
        }
    if (std::filesystem::exists(Db("pc/mw-vanilla"))) CHECK(DetectTarget(Db("pc/mw-vanilla")).reason.find("64-bit") == std::string::npos);
    // One pack of a folder says the same (gameplay.lzc is compressed).
    CHECK_EQ(DetectTarget(Db("pc/prostreet-vanilla/gameplay.lzc")).target.id(), std::string("prostreet_pc"));
    CHECK(DetectTarget(Db("ps2/prostreet-vanilla/gameplay.bin")).certain);
}

TEST("game: Most Wanted's attributes.bin names its platform: byte order, bool width, and the PC's timeofdaylighting class") {
    // Folder names that say nothing.
    const std::map<std::string, std::string> want = {{"pc/mw-vanilla/attributes.bin", "mw_pc"},
                                                      {"xbox/mw-vanilla/ATTRIBUTES.BIN", "mw_xbox"},
                                                      {"gamecube/mw-vanilla/ATTRIBUTES.BIN", "mw_gamecube"},
                                                      {"360/mw-beta/attributes.bin", "mw_x360"},
                                                      {"ps2/mw-beta/attributes.bin", "mw_ps2"}};
    for (const auto& [file, id] : want) {
        const Detection d = DetectTarget("somewhere/attributes.bin", ReadFile(Db(file)));
        CHECK_EQ(d.target.id(), id);
        CHECK(d.certain);
        CHECK(d.reason.find("no Vers chunk") != std::string::npos);
    }
    CHECK(DetectTarget("somewhere/attributes.bin", ReadFile(Db("xbox/mw-vanilla/ATTRIBUTES.BIN"))).reason.find("as on the Xbox") != std::string::npos);
    // Another pack alone has no type table: Most Wanted, the platform a guess.
    const Detection g = DetectTarget("somewhere/gameplay.bin", ReadFile(Db("pc/mw-vanilla/gameplay.bin")));
    CHECK(g.target.game == Game::MostWanted);
    CHECK(!g.certain);
}

TEST("game: without a known Vers, the chunks, the type table and the folder names guess") {
    // Pro Street's types and 4-byte bools: the PS2's.
    Detection d = DetectTarget("somewhere/attributes.bin", WithUnknownVers(Db("ps2/prostreet-vanilla/ATTRIBUTES.BIN")));
    CHECK(!d.certain);
    CHECK_EQ(d.target.id(), std::string("prostreet_ps2"));
    CHECK(d.reason.find("isn't one Attribulina knows") != std::string::npos);
    CHECK(d.reason.find("4-byte bools") != std::string::npos);
    // Carbon's types, big-endian, 1-byte bools: the 360 unless the folder says Wii.
    d = DetectTarget("somewhere/attributes.bin", WithUnknownVers(Db("wii/carbon-vanilla/attributes.bin")));
    CHECK_EQ(d.target.id(), std::string("carbon_x360"));
    d = DetectTarget("Games/NFSC Wii/attributes.bin", WithUnknownVers(Db("wii/carbon-vanilla/attributes.bin")));
    CHECK_EQ(d.target.id(), std::string("carbon_wii"));
    CHECK(d.reason.find("the folder is named nfsc wii") != std::string::npos);
    // A folder name that doesn't fit the byte order doesn't count.
    d = DetectTarget("pc/attributes.bin", WithUnknownVers(Db("wii/carbon-vanilla/attributes.bin")));
    CHECK_EQ(d.target.id(), std::string("carbon_x360"));
    // A Vers value two platforms share: attributes.bin's classes settle it,
    // other packs alone don't (the folder's name then, or the prompt).
    if (std::filesystem::exists(Db("ps3/carbon-vanilla"))) {
        d = DetectTarget("x/attributes.bin", ReadFile(Db("ps3/carbon-vanilla/ATTRIBUTES.BIN")));
        CHECK(d.certain);
        CHECK_EQ(d.target.id(), std::string("carbon_ps3"));
        d = DetectTarget("x/GAMEPLAY.BIN", ReadFile(Db("ps3/carbon-vanilla/GAMEPLAY.BIN")));
        CHECK(!d.certain);
        CHECK(d.platformWeak);
        CHECK_EQ(d.target.id(), std::string("carbon_wii"));
        CHECK(d.reason.find("Carbon's on Wii and PS3") != std::string::npos);
        d = DetectTarget("ps3/GAMEPLAY.BIN", ReadFile(Db("ps3/carbon-vanilla/GAMEPLAY.BIN")));
        CHECK(!d.certain && !d.platformWeak);
        CHECK_EQ(d.target.id(), std::string("carbon_ps3"));
    }
    // Undercover's chunk order.
    d = DetectTarget("x/attributes.bin", WithUnknownVers(Db("pc/undercover-vanilla/ATTRIBUTES.BIN")));
    CHECK_EQ(d.target.id(), std::string("undercover_pc"));
    // The Wii's and the PS2's Undercover: Carbon's chunk order, its own types,
    // and of those two platforms the big-endian one.
    d = DetectTarget("x/attributes.bin", WithUnknownVers(Db("wii/undercover-vanilla/attributes.bin")));
    CHECK(!d.certain && !d.platformWeak);
    CHECK_EQ(d.target.id(), std::string("undercover_wii"));
    CHECK(d.reason.find("its types are Undercover's (the Wii's and the PS2's)") != std::string::npos);
    // Nothing readable: the folder names, else Pro Street on PC.
    d = DetectTarget("Need for Speed Carbon/GLOBAL/attributes.bin", ByteSpan{});
    CHECK(!d.certain);
    CHECK_EQ(d.target.id(), std::string("carbon_pc"));
    CHECK_EQ(DetectTarget("nowhere", ByteSpan{}).target.id(), std::string("prostreet_pc"));
    // The chosen platform against the files: byte order, and the bools.
    const Detection be = DetectTarget(Db("360/prostreet-vanilla"));
    CHECK(Contradiction({Game::ProStreet, Platform::PC}, be).find("big-endian") != std::string::npos);
    CHECK(Contradiction({Game::ProStreet, Platform::Wii}, be).empty());
    const Detection ps2 = DetectTarget(Db("ps2/prostreet-vanilla"));
    CHECK_EQ(ps2.boolSize, 4u);
    CHECK_EQ(Contradiction({Game::ProStreet, Platform::PC}, ps2), std::string("these files' bools are 4 bytes, and PC's are 1"));
    // How sure a guess's platform is: the bools settle the PS2; Carbon's
    // big-endian files could be three platforms' unless the folder says.
    CHECK(!DetectTarget("x/attributes.bin", WithUnknownVers(Db("ps2/prostreet-vanilla/ATTRIBUTES.BIN"))).platformWeak);
    CHECK(DetectTarget("x/attributes.bin", WithUnknownVers(Db("wii/carbon-vanilla/attributes.bin"))).platformWeak);
    CHECK(!DetectTarget("wii/attributes.bin", WithUnknownVers(Db("wii/carbon-vanilla/attributes.bin"))).platformWeak);
}

TEST("game: the target picks the type schema; another game's never applies") {
    auto pc = TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt"));
    auto ps2 = TypeSchema::Load(test::DataPath("data/schema/prostreet_ps2.txt"));
    CHECK(pc->game == Game::ProStreet && ps2->game == Game::ProStreet);
    CHECK(pc->platforms == (std::vector<Platform>{Platform::PC, Platform::Xbox360, Platform::Wii}));
    auto load = [&](const std::string& file, std::optional<Target> t) {
        auto db = std::make_unique<Database>();
        if (t) db->setTarget(*t);
        db->addTypeSchema(pc);
        db->addTypeSchema(ps2);
        db->loadPack(file);
        return db;
    };
    // Detected when not set.
    auto d = load(Db("360/prostreet-vanilla/ATTRIBUTES.BIN"), std::nullopt);
    CHECK_EQ(d->target().id(), std::string("prostreet_x360"));
    CHECK(d->typeSchema() == pc.get());
    // A platform no schema lays out takes its game's best fit.
    d = load(Db("360/prostreet-vanilla/ATTRIBUTES.BIN"), Target{Game::ProStreet, Platform::PS3});
    CHECK(d->typeSchema() == pc.get());
    // The Wii's files have the PS2's Vers value and the PC's layouts.
    if (std::filesystem::exists(Db("wii/prostreet-vanilla"))) {
        d = load(Db("wii/prostreet-vanilla/ATTRIBUTES.BIN"), std::nullopt);
        CHECK_EQ(d->target().id(), std::string("prostreet_wii"));
        CHECK(d->typeSchema() == pc.get());
    }
    // The chosen platform wins, even over a better fit.
    d = load(Db("ps2/prostreet-vanilla/ATTRIBUTES.BIN"), Target{Game::ProStreet, Platform::PC});
    CHECK(d->typeSchema() == pc.get());
    // Without Carbon's schemas, Pro Street's isn't applied to Carbon's files.
    if (std::filesystem::exists(Db("pc/carbon-vanilla"))) {
        d = load(Db("pc/carbon-vanilla/attributes.bin"), std::nullopt);
        CHECK_EQ(d->target().id(), std::string("carbon_pc"));
        CHECK(d->typeSchema() == nullptr);
    }
    // Carbon's: the PC's layouts are the PS3's and the Wii's, the PS2's the
    // GameCube's. The 360's and the PS2 demo's types are older (Most
    // Wanted's StringKey). Undercover's Wii and PS2 version has a schema each,
    // its current-gen version one for the PC and the 360.
    std::map<std::string, std::shared_ptr<const TypeSchema>> all;
    for (const char* s : {"prostreet_pc", "prostreet_ps2", "carbon_pc", "carbon_ps2", "carbon_x360", "carbon_ps2_demo", "undercover_wii",
                          "undercover_ps2", "undercover_pc"})
        all[s] = TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt"));
    CHECK(all["carbon_pc"]->game == Game::Carbon);
    CHECK(all["carbon_pc"]->platforms == (std::vector<Platform>{Platform::PC, Platform::PS3, Platform::Wii}));
    CHECK(all["carbon_ps2"]->platforms == (std::vector<Platform>{Platform::PS2, Platform::GameCube}));
    CHECK(all["carbon_x360"]->platforms == std::vector<Platform>{Platform::Xbox360});
    CHECK(all["carbon_ps2_demo"]->platforms == std::vector<Platform>{Platform::PS2});
    CHECK(all["undercover_wii"]->game == Game::Undercover);
    CHECK(all["undercover_wii"]->platforms == std::vector<Platform>{Platform::Wii});
    CHECK(all["undercover_ps2"]->platforms == std::vector<Platform>{Platform::PS2});
    CHECK(all["undercover_pc"]->platforms == (std::vector<Platform>{Platform::PC, Platform::Xbox360}));
    auto loadAll = [&](const std::string& file, std::optional<Target> t) {
        auto db = std::make_unique<Database>();
        if (t) db->setTarget(*t);
        for (const auto& [name, s] : all) db->addTypeSchema(s);
        db->loadPack(file);
        return db;
    };
    for (const auto& [folder, schema] : std::vector<std::pair<std::string, std::string>>{
             {"pc/carbon-vanilla/attributes.bin", "carbon_pc"},
             {"ps3/carbon-vanilla/ATTRIBUTES.BIN", "carbon_pc"},
             {"gamecube/carbon-vanilla/ATTRIBUTES.BIN", "carbon_ps2"},
             // Two schemas lay out the PS2: the one that fits the files wins.
             {"ps2/carbon-vanilla/attributes.bin", "carbon_ps2"},
             {"ps2/carbon-demo/attributes.bin", "carbon_ps2_demo"},
             {"360/carbon-latedemo/ATTRIBUTES.BIN", "carbon_x360"},
             {"wii/undercover-vanilla/attributes.bin", "undercover_wii"},
             {"pc/undercover-vanilla/ATTRIBUTES.BIN", "undercover_pc"},
             {"360/undercover-vanilla/ATTRIBUTES.BIN", "undercover_pc"},
             {"pc/world-final/attributes.bin", "world_pc"}}) {
        if (!std::filesystem::exists(Db(folder))) continue;
        d = loadAll(Db(folder), std::nullopt);
        CHECK(d->typeSchema() == all[schema].get());
    }
    // An older header without the game takes it from the file name.
    auto old = TypeSchema::Parse("schema \"Old\" pc\nstruct A 4 4\n  x int32 0\nend\n", "data/schema/carbon_pc.txt");
    CHECK(old->game == Game::Carbon);
    bool threw = false;
    try {
        TypeSchema::Parse("schema prostreet \"T\" dreamcast\n", "x.txt");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}
