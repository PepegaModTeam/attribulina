// attribcli (src/cli/Cli.h): applying scripts to a game folder and writing the
// changed packs elsewhere, making scripts from databases, reading values.

#include "Test.h"

#include "cli/Cli.h"
#include "core/Compression.h"
#include "core/Hash.h"
#include "core/MemoryFile.h"
#include "core/ModList.h"
#include "core/Script.h"

#include <filesystem>
#include <fstream>
#include <map>
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

TEST("cli: repair saves what opening repaired in files OGVI saved (VaultLib); World's event vaults go back to gc.vaults") {
    const fs::path tmp = fs::temp_directory_path() / "attribulina_cli_repair_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    Cli run(tmp);
    // Retail files have nothing to repair.
    for (const char* folder : {"pc/mw-vanilla", "pc/undercover-vanilla"}) {
        const std::string retail = test::DataPath(std::string("attrib/databases/") + folder);
        if (!fs::exists(retail)) continue;
        CHECK_EQ(run({"repair", retail, "--dry-run"}), 0);
        CHECK(run.out.find("nothing to repair") != std::string::npos);
    }
    for (const char* name : {"mw-ogvi", "undercover-ogvi", "world-final-ogvi"}) {
        const fs::path from = test::DataPath(std::string("attrib/databases/pc/") + name);
        if (!fs::exists(from)) continue;
        const fs::path dir = tmp / name;
        fs::copy(from, dir, fs::copy_options::recursive);
        const bool world = std::string(name) == "world-final-ogvi";
        CHECK_EQ(run({"repair", dir.string(), "--dry-run"}), 0);
        CHECK(run.out.find("would write") != std::string::npos);
        CHECK(!world || run.out.find("would remove") != std::string::npos);
        CHECK_EQ(run({"repair", dir.string()}), 0);
        std::printf("        %s: %s", name, run.out.substr(0, run.out.find('\n') + 1).c_str());
        CHECK(run.out.find("repaired ") != std::string::npos);
        // Saved as the game's files, nothing is left to repair, and the folder
        // says which game it is again.
        CHECK_EQ(run({"repair", dir.string()}), 0);
        CHECK(run.out.find("nothing to repair") != std::string::npos);
        CHECK(run.err.find("warning") == std::string::npos || world); // World's coop_pursuit_1 keeps its own warnings
        if (world) {
            // The event vaults OGVI put beside attributes.bin are in gc.vaults again
            // as event vault files, and theirs are gone.
            size_t root = 0;
            for (const auto& e : fs::directory_iterator(dir))
                root += e.path().extension() == ".bin" && e.path().stem() != "attributes" && e.path().stem() != "commerce" && e.path().stem() != "FE_ATTRIB";
            CHECK_EQ(root, size_t(0));
            CHECK(attrib::ParseVaultFileHeader(attrib::ReadFile((dir / "gc.vaults" / "1_2_1_sprint.bin").string())).has_value());
        }
    }
    fs::remove_all(tmp);
}

TEST("cli: apply updates the copy of a saved pack in the game's memory files; --no-memory-files and --out") {
    const std::string vanilla = test::DataPath("attrib/databases/pc/prostreet-vanilla");
    const std::string memories = test::DataPath("attrib/memoryfiles/pc/prostreet");
    const fs::path root = fs::temp_directory_path() / "attribulina_cli_memory";
    const fs::path global = root / "GLOBAL";
    fs::remove_all(root);
    fs::create_directories(global);
    for (const char* f : {"ATTRIBUTES.BIN", "FE_ATTRIB.BIN"}) fs::copy_file(fs::path(vanilla) / f, global / f);
    for (const auto& e : fs::directory_iterator(memories)) fs::copy_file(e.path(), global / e.path().filename());
    Cli run(root);
    const fs::path memory = global / "GlobalMemoryFile.bin";
    const attrib::Bytes original = attrib::ReadFile(memory.string());
    const uint32_t attributes = attrib::GameFileHash("GLOBAL\\ATTRIBUTES.BIN");
    auto copyOf = [&](const fs::path& m) { return attrib::ReadMemoryFile(attrib::ReadFile(m.string()))->find(attributes)->bytes; };

    // Opening the folder loads the packs alone.
    CHECK_EQ(run({"info", global.string()}), 0);
    CHECK(run.out.find("MemoryFile") == std::string::npos);

    WriteText(root / "mod.amod", "attribulina 1\ngame prostreet_pc\nedit audio_revlimiter/default { total_frames_short = 9 }\n");
    const std::string mod = (root / "mod.amod").string();
    CHECK_EQ(run({"apply", global.string(), mod, "--dry-run"}), 0);
    CHECK(run.out.find("would update GlobalMemoryFile.bin's copy of GLOBAL\\ATTRIBUTES.BIN") != std::string::npos);
    CHECK(attrib::ReadFile(memory.string()) == original);

    // --out: the packs and the memory file go there; the originals stay.
    const fs::path out = root / "out";
    CHECK_EQ(run({"apply", global.string(), mod, "--out", out.string()}), 0);
    CHECK(run.out.find("updated " + (out / "GlobalMemoryFile.bin").string()) != std::string::npos);
    CHECK(attrib::ReadFile(memory.string()) == original);
    CHECK(copyOf(out / "GlobalMemoryFile.bin") == attrib::ReadFile((out / "ATTRIBUTES.BIN").string()));
    CHECK(!fs::exists(out / "InGameMemoryFile.bin")); // it holds no pack

    // --no-memory-files: the memory file stays, with a warning.
    const fs::path off = root / "off";
    CHECK_EQ(run({"apply", global.string(), mod, "--out", off.string(), "--no-memory-files"}), 0);
    CHECK(!fs::exists(off / "GlobalMemoryFile.bin"));
    CHECK(run.err.find("GlobalMemoryFile.bin holds another copy of GLOBAL\\ATTRIBUTES.BIN") != std::string::npos);

    // In place: the copy is the saved pack, and the first original is kept.
    CHECK_EQ(run({"apply", global.string(), mod}), 0);
    CHECK(run.out.find("updated " + memory.string() + ": its copy of GLOBAL\\ATTRIBUTES.BIN") != std::string::npos);
    const attrib::Bytes saved = attrib::ReadFile((global / "ATTRIBUTES.BIN").string());
    CHECK(saved != attrib::ReadFile((fs::path(vanilla) / "ATTRIBUTES.BIN").string()));
    CHECK(copyOf(memory) == saved);
    CHECK(fs::exists(global / "GlobalMemoryFile.bin.bak") && attrib::ReadFile((global / "GlobalMemoryFile.bin.bak").string()) == original);
    const auto before = attrib::ReadMemoryFile(original), after = attrib::ReadMemoryFile(attrib::ReadFile(memory.string()));
    CHECK_EQ(after->entries.size(), before->entries.size());
    for (size_t i = 0; i < before->entries.size(); ++i)
        if (before->entries[i].hash != attributes) CHECK(after->entries[i].bytes == before->entries[i].bytes);
    // The same edit again saves nothing.
    const attrib::Bytes now = attrib::ReadFile(memory.string());
    CHECK_EQ(run({"apply", global.string(), mod}), 0);
    CHECK(run.out.find("nothing changed") != std::string::npos);
    CHECK(attrib::ReadFile(memory.string()) == now);
    // Another value: the copy follows again, and the .bak stays the first original.
    WriteText(root / "mod2.amod", "attribulina 1\ngame prostreet_pc\nedit audio_revlimiter/default { total_frames_short = 11 }\n");
    CHECK_EQ(run({"apply", global.string(), (root / "mod2.amod").string()}), 0);
    CHECK(copyOf(memory) == attrib::ReadFile((global / "ATTRIBUTES.BIN").string()));
    CHECK(attrib::ReadFile((global / "GlobalMemoryFile.bin.bak").string()) == original);
    fs::remove_all(root);
}

TEST("cli: saving a pack changes nothing in the memory files but its copy, in every PC game") {
    // Every other copy keeps its bytes, FileSize, MemorySize (partial ones stay
    // partial), Data and place in the table; only the copies after the changed
    // one move, all by the same multiple of 16. Memory files holding no copy of
    // the pack aren't written, and the original pack back gives the original
    // memory file back.
    const struct {
        const char* game;
        const char* db;
        const char* target;
        const char* pack;
    } games[] = {
        {"mw", "mw-vanilla", "mw_pc", "attributes.bin"},
        {"carbon", "carbon-vanilla", "carbon_pc", "attributes.bin"},
        {"prostreet", "prostreet-vanilla", "prostreet_pc", "ATTRIBUTES.BIN"},
        {"undercover", "undercover-vanilla", "undercover_pc", "ATTRIBUTES.BIN"},
    };
    for (const auto& g : games) {
        const fs::path root = fs::temp_directory_path() / "attribulina_cli_memory_untouched";
        const fs::path global = root / "GLOBAL";
        fs::remove_all(root);
        fs::create_directories(global);
        fs::copy_file(test::DataPath("attrib/databases/pc/" + std::string(g.db) + "/" + g.pack), global / g.pack);
        std::map<std::string, attrib::Bytes> before;
        for (const auto& e : fs::directory_iterator(test::DataPath("attrib/memoryfiles/pc/" + std::string(g.game)))) {
            fs::copy_file(e.path(), global / e.path().filename());
            before[e.path().filename().string()] = attrib::ReadFile(e.path().string());
        }
        Cli run(root);
        WriteText(root / "mod.amod", std::string("attribulina 1\ngame ") + g.target + "\ncollection pvehicle/memory_file_check under pvehicle/default\n");
        CHECK_EQ(run({"apply", global.string(), (root / "mod.amod").string(), "--no-backup"}), 0);
        const attrib::Bytes pack = attrib::ReadFile((global / g.pack).string());
        const attrib::Bytes oldPack = attrib::ReadFile(test::DataPath("attrib/databases/pc/" + std::string(g.db) + "/" + g.pack));
        CHECK(pack.size() > oldPack.size());
        const uint32_t hash = attrib::GameFileHash(std::string("GLOBAL\\") + g.pack);
        for (const auto& [name, old] : before) {
            const attrib::Bytes now = attrib::ReadFile((global / name).string());
            if (name != "GlobalMemoryFile.bin") {
                CHECK(now == old); // no copy of the pack: not written
                continue;
            }
            const auto a = attrib::ReadMemoryFile(old), b = attrib::ReadMemoryFile(now);
            CHECK(a && b);
            CHECK(std::equal(old.begin(), old.begin() + 16, now.begin())); // header
            CHECK_EQ(b->entries.size(), a->entries.size());
            const auto offset = [](const attrib::Bytes& f, size_t i) { return attrib::load32(f.data() + 16 + 20 * i + 4, attrib::Endian::Little); };
            size_t changed = a->entries.size(), partial = 0;
            int64_t shift = 0;
            for (size_t i = 0; i < a->entries.size(); ++i) {
                const attrib::MemoryFileEntry &x = a->entries[i], &y = b->entries[i];
                CHECK_EQ(y.hash, x.hash);
                CHECK_EQ(y.data, x.data);
                if (x.hash == hash) {
                    CHECK(y.bytes == pack && y.whole());
                    changed = i;
                    const size_t start = offset(old, i);
                    shift = int64_t((start + pack.size() + 15) / 16 * 16) - int64_t((start + oldPack.size() + 15) / 16 * 16);
                    CHECK_EQ(offset(now, i), offset(old, i));
                    continue;
                }
                CHECK(y.bytes == x.bytes);
                CHECK_EQ(y.fileSize, x.fileSize);
                partial += !x.whole();
                CHECK_EQ(int64_t(offset(now, i)), int64_t(offset(old, i)) + (i > changed ? shift : 0));
            }
            CHECK(changed < a->entries.size());
            CHECK(partial > 0); // DYNTEX.BIN and the car texture packs: still partial
            CHECK_EQ(shift % 16, int64_t(0));
            // Byte for byte: everything before the changed copy is as it was
            // except the moved copies' Offset fields and the copy's sizes, and
            // everything after it is the old bytes moved by `shift`.
            const size_t start = offset(old, changed);
            for (size_t k = 16 + 20 * a->entries.size(); k < start; ++k) CHECK_EQ(now[k], old[k]);
            const size_t oldEnd = (start + oldPack.size() + 15) / 16 * 16, newEnd = (start + pack.size() + 15) / 16 * 16;
            CHECK_EQ(now.size() - newEnd, old.size() - oldEnd);
            CHECK(std::equal(old.begin() + oldEnd, old.end(), now.begin() + newEnd));
            // The original pack back: the original memory file.
            const auto back = attrib::PlanMemoryFileUpdates({{(global / g.pack).string(), oldPack}});
            CHECK_EQ(back.size(), size_t(1));
            CHECK(back[0].bytes == old);
        }
        fs::remove_all(root);
    }
}

TEST("cli: apply puts each script file on the game's list of installed mods once saved; again warns; --no-mod-list, --out, --dry-run") {
    const std::string vanilla = test::DataPath("attrib/databases/pc/prostreet-vanilla");
    const fs::path root = fs::temp_directory_path() / "attribulina_cli_modlist";
    const fs::path global = root / "GLOBAL";
    fs::remove_all(root);
    fs::create_directories(global);
    for (const char* f : {"ATTRIBUTES.BIN", "FE_ATTRIB.BIN"}) fs::copy_file(fs::path(vanilla) / f, global / f);
    for (const auto& e : fs::directory_iterator(test::DataPath("attrib/memoryfiles/pc/prostreet"))) fs::copy_file(e.path(), global / e.path().filename());
    Cli run(root);
    const std::string tune = "attribulina 1\ntitle \"Tune\"\nversion \"1\"\noption frames integer \"Frames\" default 5 min 1 max 50\n"
                             "edit audio_revlimiter/default { total_frames_short = $frames }\n";
    WriteText(root / "tune.amod", tune);
    WriteText(root / "long.nfsms", "version 4.6\ngame PS\n// amod begin\n// title \"Long\"\n// amod end\n"
                                   "update_field audio_revlimiter default total_frames_long 77\n");
    const std::string mod = (root / "tune.amod").string();
    auto list = [&]() { return attrib::ReadModList(global.string()); };

    CHECK_EQ(run({"apply", global.string(), mod, "--dry-run"}), 0);
    CHECK(run.out.find("would add Tune 1 to " + attrib::ModListPath(global.string())) != std::string::npos);
    CHECK(!list());

    CHECK_EQ(run({"apply", global.string(), mod, "--set", "frames=7"}), 0);
    CHECK(run.out.find("added Tune 1 to " + attrib::ModListPath(global.string())) != std::string::npos);
    auto l = list();
    CHECK(l && l->mods.size() == 1);
    if (l && l->mods.size() == 1) {
        const attrib::InstalledMod& m = l->mods[0];
        CHECK_EQ(m.title, std::string("Tune"));
        CHECK_EQ(m.sha256, attrib::Sha256Hex(tune));
        CHECK(m.by.rfind("attribcli ", 0) == 0);
        CHECK_EQ(m.game, std::string("prostreet_pc"));
        CHECK(m.options == (std::vector<std::pair<std::string, std::string>>{{"frames", "7"}}));
        CHECK(m.files == (std::vector<std::string>{"GLOBAL\\ATTRIBUTES.BIN", "GLOBAL\\GlobalMemoryFile.bin"}));
    }

    // Again: a warning, and a second entry.
    CHECK_EQ(run({"apply", global.string(), mod}), 0);
    CHECK(run.err.find("attribcli: warning: Tune 1 is installed here already") != std::string::npos);
    CHECK(list()->mods.size() == 2);

    // An nfsms script by its // amod block; -e statements are no mod.
    CHECK_EQ(run({"apply", global.string(), (root / "long.nfsms").string(), "-e", "edit audio_revlimiter/default { total_frames_short = 3 }"}), 0);
    l = list();
    CHECK(l && l->mods.size() == 3 && l->mods[2].title == "Long" && l->mods[2].nfsms);

    CHECK_EQ(run({"apply", global.string(), mod, "--no-mod-list"}), 0);
    CHECK(run.err.find("installed here already") == std::string::npos);
    CHECK(list()->mods.size() == 3);

    // --out: the list with this one goes there; the game's stays.
    const fs::path out = root / "out";
    CHECK_EQ(run({"apply", global.string(), mod, "--out", out.string()}), 0);
    const auto outList = attrib::ReadModList(out.string());
    CHECK(outList && outList->mods.size() == 4);
    CHECK(list()->mods.size() == 3);
    fs::remove_all(root);
}
