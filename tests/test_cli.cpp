// attribcli (src/cli/Cli.h): applying scripts to a game folder and writing the
// changed packs elsewhere, making scripts from databases, reading values.

#include "Test.h"

#include "cli/Cli.h"
#include "core/Compression.h"
#include "core/Script.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

struct Cli {
    cli::Env env;
    std::string out, err;

    explicit Cli(const fs::path& exeDir) {
        env.exeDir = exeDir.string(); // user_keys.txt goes there
        env.dataDir = test::DataPath("data");
    }

    int operator()(const std::vector<std::string>& args) {
        std::ostringstream o, e;
        const int code = cli::Run(args, o, e, env);
        out = o.str();
        err = e.str();
        return code;
    }
};

void WriteText(const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }

std::string ReadText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

} // namespace

TEST("cli: apply a script with options, then diff, get and export") {
    const std::string vanilla = test::DataPath("attrib/databases/pc/prostreet-vanilla");
    const fs::path tmp = fs::temp_directory_path() / "attribulina_cli_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    Cli run(tmp);

    WriteText(tmp / "mod.amod", "attribulina 1\n"
                                "game prostreet_pc\n"
                                "title \"CLI test\"\n"
                                "option frames integer \"Frames\" default 5 min 1 max 50\n"
                                "edit audio_revlimiter/default { total_frames_short = $frames }\n"
                                "collection pvehicle/cli_car under pvehicle/street in vault db { HornType = 2 }\n"
                                "collection pvehicle/cli_kid under pvehicle/cli_car\n");
    const std::string mod = (tmp / "mod.amod").string();
    const std::string out = (tmp / "out").string();

    CHECK_EQ(run({"check", mod}), 0);
    CHECK(run.out.find("option frames (integer) Frames, default 5") != std::string::npos);
    CHECK_EQ(run({"check", mod, vanilla, "--set", "frames=60"}), 1); // above the maximum
    CHECK(run.err.find("above the maximum") != std::string::npos);

    CHECK_EQ(run({"apply", vanilla, mod, "--set", "frames=9", "--out", out}), 0);
    CHECK(run.out.find("2 values set, 2 collections created") != std::string::npos);
    CHECK(fs::exists(fs::path(out) / "ATTRIBUTES.BIN"));
    CHECK(!fs::exists(fs::path(out) / "gameplay.lzc")); // only what changed

    // The difference is what the script did.
    const std::string changed = (fs::path(out) / "ATTRIBUTES.BIN").string();
    CHECK_EQ(run({"diff", vanilla, changed}), 0);
    const std::string diff = run.out;
    CHECK(diff.find("total_frames_short = 9 was 3") != std::string::npos);
    CHECK(diff.find("collection pvehicle/cli_car under pvehicle/street in vault db {") != std::string::npos);
    CHECK(diff.find("delete") == std::string::npos);

    // Applied to the original, the diff gives the same database.
    WriteText(tmp / "diff.amod", diff);
    const std::string out2 = (tmp / "out2").string();
    CHECK_EQ(run({"apply", vanilla, (tmp / "diff.amod").string(), "--out", out2}), 0);
    CHECK_EQ(run({"diff", changed, (fs::path(out2) / "ATTRIBUTES.BIN").string(), "--no-was"}), 0);
    const attrib::script::Script again = attrib::script::Parse(run.out);
    size_t changes = 0;
    for (const auto& s : again.statements)
        changes += s.kind != attrib::script::Statement::Kind::Header && s.kind != attrib::script::Statement::Kind::Meta &&
                   s.kind != attrib::script::Statement::Kind::Comment;
    CHECK_EQ(changes, size_t(0));

    CHECK_EQ(run({"get", changed, "pvehicle/cli_car", "HornType"}), 0);
    CHECK(run.out.find("    HornType = 2\n") != std::string::npos);
    CHECK_EQ(run({"get", changed, "pvehicle/cli_kid", "HornType"}), 0);
    CHECK(run.out.find("    HornType = 2  # from cli_car\n") != std::string::npos);
    CHECK_EQ(run({"list", changed, "pvehicle"}), 0);
    CHECK(run.out.find("cli_car\n") != std::string::npos);

    // An export applies to its own database without changing anything.
    const std::string street = (tmp / "street.amod").string();
    CHECK_EQ(run({"export", vanilla, "pvehicle/street", "--children", "-o", street}), 0);
    CHECK(ReadText(street).find("collection pvehicle/street under ") != std::string::npos);
    CHECK_EQ(run({"check", street, vanilla}), 0);
    CHECK(run.out.find("applies: no changes") != std::string::npos);

    // A dry run saves nothing; a failing script saves nothing either.
    const std::string out3 = (tmp / "out3").string();
    CHECK_EQ(run({"apply", vanilla, "-e", "edit audio_revlimiter/default { total_frames_short = 4 }", "--dry-run", "--out", out3}), 0);
    CHECK(run.out.find("would write") != std::string::npos);
    CHECK(!fs::exists(out3));
    CHECK_EQ(run({"apply", vanilla, "-e", "edit audio_revlimiter/default { total_frames_short = 4 }", "-e",
                  "edit pvehicle/nope { HornType = 1 }", "--out", out3}),
             1);
    CHECK(run.err.find("-e#2:1:6: error: no collection pvehicle/nope") != std::string::npos);
    CHECK(!fs::exists(fs::path(out3) / "ATTRIBUTES.BIN"));

    // nfsms scripts apply too; their setup choices come from --set.
    WriteText(tmp / "old.nfsms", "version 4.6\ngame PS\n"
                                  "ui_control checkbox loud false \"Loud\"\n"
                                  "update_field audio_revlimiter default total_frames_short 21\n"
                                  "ui_option loud\nupdate_field audio_revlimiter default total_frames_long 99\nui_option\n");
    const std::string out4 = (tmp / "out4").string();
    CHECK_EQ(run({"apply", vanilla, (tmp / "old.nfsms").string(), "--set", "loud=true", "--out", out4}), 0);
    CHECK_EQ(run({"get", (fs::path(out4) / "ATTRIBUTES.BIN").string(), "audio_revlimiter/default", "total_frames_short", "total_frames_long"}), 0);
    CHECK(run.out.find("total_frames_short = 21") != std::string::npos);
    CHECK(run.out.find("total_frames_long = 99") != std::string::npos);

    // Usage mistakes.
    CHECK_EQ(run({}), 2);
    CHECK_EQ(run({"bogus"}), 2);
    CHECK_EQ(run({"apply", vanilla}), 2);
    CHECK_EQ(run({"apply", vanilla, mod, "--set", "nope=1"}), 2);
    CHECK_EQ(run({"apply", vanilla, mod, "--frobnicate"}), 2);
    fs::remove_all(tmp);
}

TEST("cli: a script's includes are read from its folder") {
    const std::string vanilla = test::DataPath("attrib/databases/pc/prostreet-vanilla");
    const fs::path tmp = fs::temp_directory_path() / "attribulina_cli_include";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "mods" / "parts");
    Cli run(tmp);
    const fs::path mods = tmp / "mods";
    WriteText(mods / "mod.amod", "attribulina 1\ninclude parts/frames.amod\n");
    WriteText(mods / "parts" / "frames.amod", "option frames integer \"Frames\" default 5\n"
                                              "edit audio_revlimiter/default { total_frames_short = $frames }\n");
    const std::string mod = (mods / "mod.amod").string();

    CHECK_EQ(run({"check", mod}), 0);
    CHECK(run.out.find("includes " + (mods / "parts" / "frames.amod").string() + "\n") != std::string::npos);
    CHECK(run.out.find("option frames (integer) Frames, default 5") != std::string::npos);
    // --set finds the included script's option.
    const std::string out = (tmp / "out").string();
    CHECK_EQ(run({"apply", vanilla, mod, "--set", "frames=12", "--out", out}), 0);
    CHECK_EQ(run({"get", (fs::path(out) / "ATTRIBUTES.BIN").string(), "audio_revlimiter/default", "total_frames_short"}), 0);
    CHECK(run.out.find("total_frames_short = 12") != std::string::npos);
    CHECK_EQ(run({"apply", vanilla, "-e", "include " + attrib::script::Quote((mods / "parts" / "frames.amod").string()), "--dry-run"}), 0);

    // A mistake in an included script names its file and line.
    WriteText(mods / "parts" / "bad.amod", "attribulina 1\nedit pvehicle/nope { HornType = 1 }\n");
    WriteText(mods / "broken.amod", "attribulina 1\ninclude parts/bad.amod\n");
    CHECK_EQ(run({"apply", vanilla, (mods / "broken.amod").string(), "--dry-run"}), 1);
    CHECK(run.err.find((mods / "parts" / "bad.amod").string() + ":2: error: no collection pvehicle/nope") != std::string::npos);
    // A missing one stops it before the database loads.
    WriteText(mods / "missing.amod", "include parts/gone.amod\n");
    CHECK_EQ(run({"check", (mods / "missing.amod").string(), vanilla}), 1);
    CHECK(run.err.find("missing.amod:1:1: include \"parts/gone.amod\": can't read") != std::string::npos);
    fs::remove_all(tmp);
}

TEST("cli: the game and platform come from the files; --target names them; nfsms runs on every platform") {
    const fs::path tmp = fs::temp_directory_path() / "attribulina_cli_target";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    Cli cli(tmp);
    const std::string ps2 = test::DataPath("attrib/databases/ps2/prostreet-vanilla"), x360 = test::DataPath("attrib/databases/360/prostreet-vanilla");
    CHECK_EQ(cli({"info", ps2}), 0);
    CHECK(cli.out.find("game: Pro Street on PS2 (prostreet_ps2), from Vers 0x6CDFE96B890BB714, little-endian\nschema: prostreet_ps2.txt\n") == 0);
    CHECK_EQ(cli({"info", x360}), 0);
    CHECK(cli.out.find("game: Pro Street on Xbox 360 (prostreet_x360), from Vers") == 0);
    CHECK(cli.out.find("schema: prostreet_pc.txt\n") != std::string::npos);
    // --target reads them as another platform (the PC's schema lays out the Wii's too, as the best fit).
    CHECK_EQ(cli({"info", x360, "--target", "prostreet_wii"}), 0);
    CHECK(cli.out.find("game: Pro Street on Wii (prostreet_wii), as --target says\n") == 0);
    CHECK_EQ(cli({"info", x360, "--target", "nfs_pc"}), 2);
    CHECK(cli.err.find("unknown --target nfs_pc") != std::string::npos);
    // Scripts made from it name it.
    CHECK_EQ(cli({"get", x360, "audio_revlimiter/default", "total_frames_short"}), 0);
    CHECK_EQ(cli({"export", x360, "audio_revlimiter/default", "-o", (tmp / "x.amod").string()}), 0);
    CHECK(ReadText(tmp / "x.amod").find("game prostreet_x360\n") != std::string::npos);
    // nfsms: NFS-VltEd's format, run on any platform of the game.
    WriteText(tmp / "a.nfsms", "update_field audio_revlimiter default total_frames_short 9\n");
    CHECK_EQ(cli({"apply", x360, (tmp / "a.nfsms").string(), "--out", (tmp / "out").string()}), 0);
    CHECK_EQ(cli({"get", (tmp / "out").string(), "audio_revlimiter/default", "total_frames_short"}), 0);
    CHECK(cli.out.find("total_frames_short = 9") != std::string::npos);
    fs::remove_all(tmp);
}

TEST("cli: apply --compress saves gameplay.lzc with another codec, and its .bin beside it") {
    const std::string vanilla = test::DataPath("attrib/databases/pc/prostreet-vanilla");
    const fs::path tmp = fs::temp_directory_path() / "attribulina_cli_compress_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    Cli run(tmp);
    const attrib::Bytes pack = attrib::ReadFile(vanilla + "/gameplay.bin");

    CHECK(run({"apply", vanilla, "--compress", "zip"}) != 0);
    CHECK(run.err.find("--compress takes jdlz, huff, raww or rfpk") != std::string::npos);
    for (const char* kind : {"huff", "RAWW", "rfpk"}) {
        const fs::path out = tmp / kind;
        CHECK_EQ(run({"apply", vanilla, "--compress", kind, "--out", out.string()}), 0);
        const attrib::Bytes lzc = attrib::ReadFile((out / "gameplay.lzc").string());
        const auto h = attrib::ParseLzHeader(lzc);
        CHECK(h && h->kind == attrib::ParseLzKind(kind));
        CHECK(attrib::Decompress(lzc) == pack);
        CHECK(attrib::ReadFile((out / "gameplay.bin").string()) == pack);
        CHECK(!fs::exists(out / "ATTRIBUTES.BIN")); // only the pack it changed
    }
    // Retail's is JDLZ already.
    CHECK_EQ(run({"apply", vanilla, "--compress", "jdlz", "--dry-run"}), 0);
    CHECK(run.out.find("nothing would change") != std::string::npos);
}

TEST("cli: a class made by a script saves and loads; header writes its Attrib::Gen class; the game's stay as they are") {
    const fs::path tmp = fs::temp_directory_path() / "attribulina_cli_class_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "out");
    const std::string attrs = test::DataPath("attrib/databases/pc/prostreet-vanilla/ATTRIBUTES.BIN");
    Cli run(tmp);
    WriteText(tmp / "mod.amod", "attribulina 1\n"
                                "game prostreet_pc\n"
                                "class cli_class {\n"
                                "    layout SPEED Float\n"
                                "    entry NAMES StringKey[]\n"
                                "}\n"
                                "collection cli_class/base under root in vault db { SPEED = 2 }\n");
    CHECK_EQ(run({"apply", attrs, (tmp / "mod.amod").string(), "--out", (tmp / "out").string(), "--no-backup"}), 0);
    const std::string saved = (tmp / "out" / "ATTRIBUTES.BIN").string();
    CHECK(fs::exists(saved));
    CHECK_EQ(run({"get", saved, "cli_class/base"}), 0);
    CHECK(run.out.find("SPEED = 2") != std::string::npos);
    CHECK_EQ(run({"header", saved, "cli_class", "-o", (tmp / "cli_class.h").string()}), 0);
    const std::string h = ReadText(tmp / "cli_class.h");
    CHECK(h.find("class cli_class : private Instance") != std::string::npos);
    CHECK(h.find("unsigned int Num_NAMES() const") != std::string::npos);
    // A game class can't be redefined from a script.
    CHECK(run({"apply", attrs, "--dry-run", "-e", "class pvehicle {\n    entry MOD Float\n}"}) != 0);
    CHECK(run.err.find("class of the game") != std::string::npos);
    fs::remove_all(tmp);
}
