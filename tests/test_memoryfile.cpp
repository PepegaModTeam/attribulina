// The game's memory files (core/MemoryFile.h): every PC one read and written
// back byte for byte, the path hash bFile uses, and what saving a pack does to
// the memory files beside it.

#include "Test.h"

#include "core/MemoryFile.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace attrib;
namespace fs = std::filesystem;

namespace {

std::string MemoryDir(const std::string& game) { return test::DataPath("attrib/memoryfiles/pc/" + game); }

std::vector<std::string> AllMemoryFiles() {
    std::vector<std::string> out;
    for (const char* g : {"mw", "carbon", "prostreet", "undercover"})
        for (const auto& e : fs::directory_iterator(MemoryDir(g))) out.push_back(e.path().string());
    std::sort(out.begin(), out.end());
    return out;
}

// A folder laid out as the game's Global: the memory files of `game` and the
// listed files of its retail database folder.
fs::path GlobalCopy(const std::string& name, const std::string& game, const std::string& db, const std::vector<std::string>& files) {
    const fs::path dir = fs::temp_directory_path() / name / "GLOBAL";
    fs::remove_all(dir.parent_path());
    fs::create_directories(dir);
    for (const auto& e : fs::directory_iterator(MemoryDir(game))) fs::copy_file(e.path(), dir / e.path().filename());
    for (const std::string& f : files) fs::copy_file(test::DataPath("attrib/databases/pc/" + db + "/" + f), dir / f);
    return dir;
}

} // namespace

TEST("memoryfile: every PC memory file reads and writes back byte for byte, its copies 16-aligned") {
    const std::vector<std::string> files = AllMemoryFiles();
    CHECK_EQ(files.size(), size_t(18));
    size_t entries = 0;
    for (const std::string& f : files) {
        const Bytes data = ReadFile(f);
        const std::optional<MemoryFile> mf = ReadMemoryFile(data);
        CHECK(mf);
        CHECK(mf->endian == Endian::Little);
        entries += mf->entries.size();
        const Bytes again = WriteMemoryFile(*mf);
        if (again != data) std::printf("        %s differs\n", f.c_str());
        CHECK(again == data);
    }
    CHECK_EQ(entries, size_t(849));
    // Not a memory file.
    CHECK(!ReadMemoryFile(ReadFile(test::DataPath("attrib/databases/pc/carbon-vanilla/attributes.bin"))));
    // A table running past the end.
    Bytes broken = ReadFile(MemoryDir("carbon") + "/InGameMemoryFile.bin");
    broken.resize(100);
    bool threw = false;
    try {
        (void)ReadMemoryFile(broken);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST("memoryfile: the path hash and the game paths of files beside a memory file") {
    // bFileGetFilenameHash: upper case, '/' as '\'.
    CHECK_EQ(GameFileHash("GLOBAL\\ATTRIBUTES.BIN"), 0x9775379Au);
    CHECK_EQ(GameFileHash("global/attributes.bin"), 0x9775379Au);
    CHECK_EQ(GameFileHash("GLOBAL\\CARS_VAULT.BIN"), 0xA83B0DC7u);
    CHECK_EQ(GameFileHash("GLOBAL\\GAMEPLAY.LZC"), 0x51EB2B73u);
    const fs::path root = fs::temp_directory_path() / "attribulina_gamepath" / "NFS";
    const std::string memory = (root / "Global" / "GlobalMemoryFile.bin").string();
    CHECK_EQ(GamePath((root / "Global" / "attributes.bin").string(), memory).value_or(""), std::string("GLOBAL\\ATTRIBUTES.BIN"));
    CHECK_EQ(GamePath((root / "Global" / "gc.vaults" / "x.bin").string(), memory).value_or(""), std::string("GLOBAL\\GC.VAULTS\\X.BIN"));
    CHECK_EQ(GamePath((root / "FrontEnd" / "x.bin").string(), memory).value_or(""), std::string("FRONTEND\\X.BIN"));
    CHECK_EQ(GamePath((root / "x.bin").string(), memory).value_or(""), std::string("X.BIN"));
    CHECK(!GamePath((root.parent_path() / "elsewhere" / "x.bin").string(), memory));
    // Whatever the folder is called here, the game loads it as Global.
    CHECK_EQ(GamePath("/a/prostreet-vanilla/ATTRIBUTES.BIN", "/a/prostreet-vanilla/GlobalMemoryFile.bin").value_or(""),
        std::string("GLOBAL\\ATTRIBUTES.BIN"));
}

TEST("memoryfile: each PC game's GlobalMemoryFile.bin holds its retail attributes.bin whole") {
    const struct {
        const char* game;
        const char* db;
        std::vector<std::string> files;
    } games[] = {
        {"mw", "mw-vanilla", {"attributes.bin"}},
        {"carbon", "carbon-vanilla", {"attributes.bin"}},
        {"prostreet", "prostreet-vanilla", {"ATTRIBUTES.BIN"}},
        {"undercover", "undercover-vanilla", {"ATTRIBUTES.BIN", "cars_vault.BIN", "gameplay.lzc"}},
    };
    for (const auto& g : games) {
        const std::optional<MemoryFile> mf = ReadMemoryFile(ReadFile(MemoryDir(g.game) + "/GlobalMemoryFile.bin"));
        CHECK(mf);
        for (const std::string& f : g.files) {
            const MemoryFileEntry* e = mf->find(GameFileHash("GLOBAL\\" + f));
            CHECK(e);
            CHECK(e->whole());
            CHECK(e->bytes == ReadFile(test::DataPath("attrib/databases/pc/" + std::string(g.db) + "/" + f)));
        }
        // What opening the pack reports.
        const fs::path dir = GlobalCopy("attribulina_memory_copies", g.game, g.db, g.files);
        for (const std::string& f : g.files) {
            const std::vector<MemoryFileCopy> copies = MemoryFileCopiesOf((dir / f).string());
            CHECK_EQ(copies.size(), size_t(1));
            CHECK(copies[0].same && copies[0].whole);
            CHECK_EQ(fs::path(copies[0].memoryFile).filename().string(), std::string("GlobalMemoryFile.bin"));
        }
        CHECK(MemoryFileCopiesOf((dir / "GlobalMemoryFile.bin").string()).empty());
        fs::remove_all(dir.parent_path());
    }
    // The ones named *MemoryFile.bin with the magic.
    CHECK_EQ(MemoryFilesIn(MemoryDir("prostreet")).size(), size_t(5));
    CHECK(MemoryFilesIn(test::DataPath("attrib/databases/pc/prostreet-vanilla")).empty());
}

TEST("memoryfile: a saved file's copy takes its bytes, the others keep theirs, and a removed file's copy goes") {
    const fs::path dir = GlobalCopy("attribulina_memory_plan", "undercover", "undercover-vanilla", {"ATTRIBUTES.BIN"});
    const std::string memory = (dir / "GlobalMemoryFile.bin").string();
    const Bytes original = ReadFile(memory);
    const MemoryFile before = *ReadMemoryFile(original);

    // Saved as it is: nothing to write.
    Bytes attributes = ReadFile((dir / "ATTRIBUTES.BIN").string());
    CHECK(PlanMemoryFileUpdates({{(dir / "ATTRIBUTES.BIN").string(), attributes}}).empty());

    // A memory file whose CARS_VAULT.BIN copy holds only the first 0x100 bytes,
    // as the game's own partial copies do (DYNTEX.BIN, the car texture packs).
    const uint32_t carsHash = GameFileHash("GLOBAL\\CARS_VAULT.BIN"), dyntexHash = GameFileHash("GLOBAL\\DYNTEX.BIN");
    {
        MemoryFile partial = before;
        partial.find(carsHash)->bytes.resize(0x100);
        WriteFile(memory, WriteMemoryFile(partial));
    }
    const Bytes withPartial = ReadFile(memory);
    const MemoryFile start = *ReadMemoryFile(withPartial);
    CHECK(!start.find(carsHash)->whole() && !start.find(dyntexHash)->whole());

    // ATTRIBUTES.BIN longer by 0x24 bytes (not a multiple of 16), and
    // CARS_VAULT.BIN saved: its partial copy becomes whole. DYNTEX.BIN's isn't a
    // saved file and stays as it is.
    attributes.insert(attributes.end(), 0x24, 0xAB);
    attributes[0x40] ^= 0xFF;
    const Bytes carsVault = ReadFile(test::DataPath("attrib/databases/pc/undercover-vanilla/cars_vault.BIN"));
    std::vector<MemoryFileUpdate> plan = PlanMemoryFileUpdates({{(dir / "ATTRIBUTES.BIN").string(), attributes}, {(dir / "CARS_VAULT.BIN").string(), carsVault}});
    CHECK_EQ(plan.size(), size_t(1));
    CHECK_EQ(plan[0].path, memory);
    CHECK((plan[0].updated == std::vector<std::string>{"GLOBAL\\ATTRIBUTES.BIN", "GLOBAL\\CARS_VAULT.BIN"}));
    CHECK(plan[0].madeWhole == std::vector<std::string>{"GLOBAL\\CARS_VAULT.BIN"});
    SaveMemoryFileUpdate(plan[0]);
    const Bytes written = ReadFile(memory);
    const std::optional<MemoryFile> after = ReadMemoryFile(written);
    CHECK(after);
    CHECK_EQ(after->entries.size(), start.entries.size());
    CHECK(WriteMemoryFile(*after) == written);
    for (size_t i = 0; i < start.entries.size(); ++i) {
        const MemoryFileEntry& b = start.entries[i];
        const MemoryFileEntry& a = after->entries[i];
        CHECK_EQ(a.hash, b.hash);
        CHECK_EQ(a.data, b.data);
        if (b.hash == GameFileHash("GLOBAL\\ATTRIBUTES.BIN")) {
            CHECK(a.bytes == attributes);
            CHECK_EQ(a.fileSize, uint32_t(attributes.size()));
        } else if (b.hash == carsHash) {
            CHECK(a.bytes == carsVault && a.whole());
        } else {
            CHECK(a.bytes == b.bytes);
            CHECK_EQ(a.fileSize, b.fileSize);
        }
    }
    CHECK(!after->find(dyntexHash)->whole());
    // Each copy at the next 16-byte boundary, the bytes between them zero.
    {
        size_t pos = 16 + 20 * after->entries.size();
        for (size_t i = 0; i < after->entries.size(); ++i) {
            const uint32_t off = load32(written.data() + 16 + 20 * i + 4, Endian::Little);
            CHECK_EQ(off % 16, 0u);
            CHECK_EQ(size_t(off), (pos + 15) / 16 * 16);
            for (size_t k = pos; k < off; ++k) CHECK_EQ(written[k], uint8_t(0));
            pos = off + after->entries[i].bytes.size();
        }
        CHECK_EQ(pos, written.size());
    }
    CHECK(MemoryFileCopiesOf((dir / "ATTRIBUTES.BIN").string())[0].same == false); // the file on disk is the old one
    WriteFile((dir / "ATTRIBUTES.BIN").string(), attributes);
    CHECK(MemoryFileCopiesOf((dir / "ATTRIBUTES.BIN").string())[0].same);
    // Again: nothing changes.
    CHECK(PlanMemoryFileUpdates({{(dir / "ATTRIBUTES.BIN").string(), attributes}, {(dir / "CARS_VAULT.BIN").string(), carsVault}}).empty());

    // The original ATTRIBUTES.BIN back, and CARS_VAULT.BIN's copy cut to its
    // first 0x100 bytes again, give the memory file as it was byte for byte.
    plan = PlanMemoryFileUpdates({{(dir / "ATTRIBUTES.BIN").string(), before.find(GameFileHash("GLOBAL\\ATTRIBUTES.BIN"))->bytes}});
    CHECK_EQ(plan.size(), size_t(1));
    MemoryFile restored = *ReadMemoryFile(plan[0].bytes);
    restored.find(carsHash)->bytes.resize(0x100);
    CHECK(WriteMemoryFile(restored) == withPartial);
    restored.find(carsHash)->bytes = carsVault;
    CHECK(WriteMemoryFile(restored) == original);

    // A removed file's copy is dropped; one in no memory file changes nothing.
    plan = PlanMemoryFileUpdates({}, {(dir / "CARS_VAULT.BIN").string(), (dir / "FE_ATTRIB.BIN").string()});
    CHECK_EQ(plan.size(), size_t(1));
    CHECK(plan[0].dropped == std::vector<std::string>{"GLOBAL\\CARS_VAULT.BIN"});
    CHECK_EQ(ReadMemoryFile(plan[0].bytes)->entries.size(), before.entries.size() - 1);
    CHECK(!ReadMemoryFile(plan[0].bytes)->find(GameFileHash("GLOBAL\\CARS_VAULT.BIN")));
    CHECK(PlanMemoryFileUpdates({{(dir / "FE_ATTRIB.BIN").string(), Bytes(10)}}).empty());

    // A file in gc.vaults has its copy in the Global folder above it.
    fs::create_directories(dir / "gc.vaults");
    CHECK_EQ(MemoryFilesFor((dir / "gc.vaults" / "x.bin").string()).size(), size_t(5));
    fs::remove_all(dir.parent_path());
}

TEST("memoryfile: a big-endian memory file writes back as one") {
    MemoryFile mf;
    mf.endian = Endian::Big;
    mf.entries.push_back({0x11111111, 3, 0x80001000, Bytes{1, 2, 3}});
    mf.entries.push_back({0x22222222, 40, 0x80002000, Bytes(17, 9)});
    const Bytes data = WriteMemoryFile(mf);
    CHECK_EQ(load32(data.data() + 8, Endian::Big), kMemoryFileMagic);
    // The table ends at 0x38: the copies start at 0x40 and 0x50.
    CHECK_EQ(load32(data.data() + 16 + 4, Endian::Big), 0x40u);
    CHECK_EQ(load32(data.data() + 16 + 20 + 4, Endian::Big), 0x50u);
    CHECK_EQ(data.size(), size_t(0x61));
    const std::optional<MemoryFile> back = ReadMemoryFile(data);
    CHECK(back && back->endian == Endian::Big);
    CHECK(WriteMemoryFile(*back) == data);
}
